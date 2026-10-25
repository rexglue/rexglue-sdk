namespace test_fixture {
namespace gpu = rex::graphics::d3d12::fixture;
using AdvanceFunction = bool (*)(uint32_t, uint64_t, uint64_t, uint64_t) noexcept;
using ConfigureFunction = bool (*)(const gpu::Config*) noexcept;
using ReportFunction = bool (*)(gpu::Report*) noexcept;
using RetireFunction = bool (*)() noexcept;
AdvanceFunction advance = nullptr;
ConfigureFunction configure = nullptr;
ReportFunction report_copy = nullptr;
RetireFunction retire = nullptr;
uint8_t* target = nullptr;
uint32_t target_address = 0;
uint32_t terminal_address = 0;
std::array<uint32_t, 6> fetch_words{};
gpu::Config config{};
gpu::Report final_report{};
bool report_ready = false, retired = false;
bool callbacks_ok = true;
uint32_t draw_count = 0, target_copies = 0;
std::array<std::vector<uint8_t>, 2> producer_bytes, producer_pixels;
struct Occurrence {
  uint64_t id = 0, frame = 0, submission = 0;
  std::vector<uint32_t> registers;
};
std::vector<Occurrence> consumers;

bool Connect() {
  const auto module = GetModuleHandleW(L"rexgpu-xenos.dll");
  if (!module)
    return false;
  configure =
      reinterpret_cast<ConfigureFunction>(GetProcAddress(module, "RexTextureFixtureConfigure"));
  advance = reinterpret_cast<AdvanceFunction>(GetProcAddress(module, "RexTextureFixtureAdvance"));
  report_copy =
      reinterpret_cast<ReportFunction>(GetProcAddress(module, "RexTextureFixtureCopyReport"));
  retire = reinterpret_cast<RetireFunction>(GetProcAddress(module, "RexTextureFixtureRetire"));
  return configure && advance && report_copy && retire;
}
bool Configure(const std::array<uint32_t, 6>& words) {
  const char* fail = std::getenv("REX_TEXTURE_FIXTURE_FAIL");
  const char* independent = std::getenv("REX_TEXTURE_FIXTURE_INDEPENDENT");
  const char* receipt = std::getenv("REX_TEXTURE_FIXTURE_RECEIPT");
  if (!fail || !independent || !receipt ||
      (std::string_view(fail) != "0" && std::string_view(fail) != "1") ||
      (std::string_view(independent) != "0" && std::string_view(independent) != "1") ||
      (std::string_view(receipt) != "0" && std::string_view(receipt) != "1"))
    return false;
  config.run = 1;
  config.fail = fail[0] == '1';
  config.independent = independent[0] == '1';
  config.receipt = receipt[0] == '1';
  std::copy(words.begin(), words.end(), config.fetch);
  return configure(&config);
}
void ObserveCopy(const rex::graphics::diagnostic::DrawCaptureFrameInputCopyView& copy) {
  capture_io::CopyReceipt(copy);
  if (copy.written_address != target_address)
    return;
  if (!copy.original_succeeded || copy.copy_mode != "resolve_readback" ||
      copy.written_length != kSurfaceBytes || target_copies >= 2) {
    callbacks_ok = false;
    return;
  }
  const auto index = target_copies++;
  producer_bytes[index].assign(target, target + kSurfaceBytes);
  callbacks_ok &= DecodeRgba8Texture(fetch_words, producer_bytes[index], kSurfaceWidth,
                                     kSurfaceHeight, producer_pixels[index]);
  callbacks_ok &= capture_io::Bytes(index ? "producer-b-tiled.bin" : "producer-a-tiled.bin",
                                    producer_bytes[index]);
  callbacks_ok &= capture_io::Bytes(index ? "producer-b-rgba.bin" : "producer-a-rgba.bin",
                                    producer_pixels[index]);
  // The first real resolve establishes the seed epoch and arms the mailbox.
  callbacks_ok &= advance(index ? 3 : 1, copy.id, copy.frame, copy.submission);
  if (!index)
    capture_io::Arm(copy);
}
void ObserveDraw(const rex::graphics::diagnostic::DrawCaptureFrameInputDrawView& draw) {
  ++draw_count;
  callbacks_ok &=
      draw.original_host_issued &&
      draw.execution_status == rex::graphics::diagnostic::DrawCaptureExecutionStatus::kIssued &&
      draw.execution_reason == rex::graphics::diagnostic::DrawCaptureReason::kNone;
  callbacks_ok &= capture_io::Bytes(
      "observer-draw-" + std::to_string(draw.id) + "-registers.bin",
      {reinterpret_cast<const uint8_t*>(draw.registers.data()), draw.registers.size_bytes()});
  if (draw_count == 1 || draw_count == 3)
    return;  // Actual A/B producers.
  consumers.push_back(
      {draw.id, draw.frame, draw.submission, {draw.registers.begin(), draw.registers.end()}});
  callbacks_ok &= advance(draw_count == 2 ? 2 : 4, draw.id, draw.frame, draw.submission);
}
void ObserveEnd(uint64_t frame, uint64_t submission, bool succeeded) {
  // OnFrameEnd releases the observer mutex after these copies, publishing them to the waiting
  // caller. No GPU wait or resource serialization occurs here.
  if (draw_count != 7 || target_copies != 2 || report_ready)
    return;
  callbacks_ok &= succeeded;
  report_ready = report_copy(&final_report);
  retired = retire();
  callbacks_ok &= report_ready && retired;
}
bool Validate(const OriginalObservation::Snapshot& observation) {
  if (!report_ready)
    return false;
  const auto& report = final_report;
  bool trace_okay = capture_io::SaveReceipt(report.trace);
  if (config.receipt) {
    trace_okay &= report.trace.enabled && report.trace.closed && !report.trace.incomplete &&
                  !report.trace.lost && !report.trace.first_lost;
    uint32_t declarations = 0, success_a = 0, success_b = 0, failures = 0, retry = 0;
    uint64_t restored_generation = 0;
    for (uint32_t i = 0; i < report.trace.count; ++i) {
      const auto& row = report.trace.rows[i];
      trace_okay &= row.sequence == i + 1;
      if (row.kind == gpu::TraceKind::kDeclaration && row.a == target_address) {
        ++declarations;
        trace_okay &= row.occurrence && row.b == kSurfaceBytes && !row.c;
      }
      if (row.texture.instance != report.seed.instance)
        continue;
      if (row.kind == gpu::TraceKind::kLoadSuccess) {
        // Warmup loads precede the A declaration and retain unseen ancestry.
        if (row.phase == gpu::Phase::kWarmup)
          continue;
        trace_okay &= row.c && row.a && !row.b && !row.texture.dirty;
        if (row.phase == gpu::Phase::kSeedA)
          ++success_a;
        else if (row.phase == gpu::Phase::kReloadB) {
          ++success_b;
          trace_okay &= !config.fail;
        } else if (row.phase == gpu::Phase::kRetryB) {
          ++retry;
          trace_okay &= config.fail && row.texture.generation == restored_generation;
        } else
          trace_okay = false;
      }
      if (row.kind == gpu::TraceKind::kBackendFailure) {
        ++failures;
        restored_generation = row.texture.generation;
        trace_okay &= config.fail && row.phase == gpu::Phase::kReloadB && row.c &&
                      row.texture.dirty == 1 && row.entry_generation + 1 == restored_generation;
      }
    }
    trace_okay &= declarations == 2 && success_a == 1 && success_b == !config.fail &&
                  failures == config.fail && retry == config.fail;
  } else {
    trace_okay &= !report.trace.enabled && !report.trace.closed && !report.trace.incomplete &&
                  !report.trace.count && !report.trace.lost && !report.trace.first_lost;
  }
  bool okay = callbacks_ok && !report.error && report.sealed && target_copies == 2 &&
              draw_count == 7 && consumers.size() == 5 && observation.copies.size() == 3 &&
              observation.swaps.size() == 3 && report.selected == (config.fail ? 1 : 3) &&
              report.post_seed_attempts == (config.fail ? 2 : 1) &&
              report.injected == config.fail && report.ordinal == config.fail && trace_okay;
  // Compare every logical pixel, including alpha, after the full CPU readback.
  for (size_t p = 0; p < 2; ++p) {
    okay &= producer_pixels[p].size() == kSurfaceBytes;
    for (size_t i = 0; i + 3 < producer_pixels[p].size(); i += 4) {
      okay &= producer_pixels[p][i] == 26 && producer_pixels[p][i + 1] == 51 &&
              producer_pixels[p][i + 2] == 77 && producer_pixels[p][i + 3] == (p ? 0 : 255);
    }
  }
  std::ofstream ledger(capture_io::root / "controller.txt", std::ios::noreplace);
  ledger << "run=" << report.config.run << " fail=" << config.fail
         << " independent=" << config.independent << " error=" << report.error
         << " sealed=" << report.sealed << " selected=" << report.selected
         << " post_seed_attempts=" << report.post_seed_attempts << " ordinal=" << report.ordinal
         << " injected=" << report.injected << " callbacks_ok=" << callbacks_ok << '\n';
  for (uint32_t i = 0; i < report.count; ++i) {
    const auto& r = report.receipts[i];
    ledger << "receipt=" << r.sequence << " kind=" << uint32_t(r.kind)
           << " phase=" << uint32_t(r.phase) << " event=" << r.event << " frame=" << r.frame
           << " submission=" << r.submission << " instance=" << r.texture.instance
           << " generation=" << r.texture.generation << " resource=" << r.texture.resource
           << " dirty=" << r.texture.dirty << " key=";
    for (uint32_t j = 0; j < r.texture.key_size; ++j)
      ledger << std::hex << std::setw(2) << std::setfill('0') << uint32_t(r.texture.key[j]);
    ledger << std::dec << " base=" << r.texture.base_address << ',' << r.texture.base_size
           << " mips=" << r.texture.mip_address << ',' << r.texture.mip_size
           << " host=" << r.texture.host_width << ',' << r.texture.host_height << ','
           << r.texture.host_depth << ',' << r.texture.host_mips << ',' << r.texture.host_format
           << " a=" << r.a << " b=" << r.b << " c=" << r.c << " d=" << r.d << '\n';
  }
  okay &= capture_io::Bytes("controller.bin",
                            {reinterpret_cast<const uint8_t*>(&report), sizeof(report)});
  const auto owned = capture_io::owned;
  bool final_source_b = false, final_output_black = false, final_layout = false;
  if (owned) {
    const auto& final = owned->final_swap;
    final_layout =
        final.frontbuffer_ptr == terminal_address &&
        final.source_availability ==
            rex::graphics::diagnostic::DrawCaptureAvailability::kComplete &&
        final.output_availability ==
            rex::graphics::diagnostic::DrawCaptureAvailability::kComplete &&
        final.source_unavailable_reason == DrawCaptureReason::kNone &&
        final.output_unavailable_reason == DrawCaptureReason::kNone &&
        final.source_format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        final.output_format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        final.source_width == kConsumerWidth && final.source_height == kConsumerHeight &&
        final.output_width == kConsumerWidth && final.output_height == kConsumerHeight &&
        final.source_row_size == kConsumerWidth * 4 &&
        final.source_row_pitch == kConsumerWidth * 4 && final.source_row_count == kConsumerHeight &&
        final.output_row_size == kConsumerWidth * 4 &&
        final.output_row_pitch == kConsumerWidth * 4 && final.output_row_count == kConsumerHeight &&
        final.source_bytes.size() == kConsumerLinearBytes &&
        final.output_bytes.size() == kConsumerLinearBytes;
    // B has zero alpha. The existing consumer multiplies its color by that
    // alpha, so every raw RGBA8 channel must be zero after the final retry.
    final_source_b =
        final_layout && std::all_of(final.source_bytes.begin(), final.source_bytes.end(),
                                    [](uint8_t byte) { return byte == 0; });
    final_output_black = final_layout;
    for (size_t i = 0; i + 3 < final.output_bytes.size(); i += 4) {
      // Little-endian RGB10A2: RGB=0, opaque A=3 is word 0xC0000000.
      final_output_black &= final.output_bytes[i] == 0 && final.output_bytes[i + 1] == 0 &&
                            final.output_bytes[i + 2] == 0 && final.output_bytes[i + 3] == 0xC0;
    }
    ledger << "final_layout=" << final_layout << " final_source_b=" << final_source_b
           << " final_output_black=" << final_output_black
           << " final_source_sha256=" << capture_io::Hash(final.source_bytes)
           << " final_output_sha256=" << capture_io::Hash(final.output_bytes) << '\n';
  }
  okay &= final_layout && final_source_b && final_output_black;
  std::vector<const rex::graphics::diagnostic::DrawCaptureFrameEvent*> events;
  if (owned) {
    for (const auto& e : owned->events)
      if (e.kind == rex::graphics::diagnostic::DrawCaptureFrameEvent::Kind::kDraw &&
          e.host_issued && e.draw.guest_pixel_shader_hash == observation.draws[1].pixel_shader_hash)
        events.push_back(&e);
  }
  okay &= events.size() == 5;
  std::vector<uint32_t> target_indices;
  for (size_t i = 0; i < events.size() && i < consumers.size(); ++i) {
    const auto& e = *events[i];
    const auto& observer = consumers[i];
    // Occurrence order is explicit. These draws intentionally repeat state and hashes.
    const bool join =
        e.host_issued && e.frame == observer.frame && e.submission == observer.submission &&
        e.draw.draw.registers == observer.registers && e.frame == consumers[0].frame &&
        e.execution_status == DrawCaptureExecutionStatus::kIssued &&
        e.execution_reason == DrawCaptureReason::kNone;
    okay &= join;
    ledger << "join occurrence=" << i << " observer_event=" << observer.id
           << " owned_event=" << e.id << " frame=" << e.frame << " submission=" << e.submission
           << " complete=" << join << '\n';
    uint32_t count = 0;
    for (const auto& use : e.draw.texture_uses) {
      if (!use.required || use.stage != 1 || use.shader_binding_index || use.fetch_constant ||
          use.is_signed)
        continue;
      ++count;
      if (config.fail && (i == 1 || i == 2)) {
        const bool unavailable =
            !use.data_available &&
            use.availability == rex::graphics::diagnostic::DrawCaptureAvailability::kUnavailable &&
            use.unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
            use.texture_index ==
                rex::graphics::diagnostic::DrawCaptureTextureUse::kUnavailableTexture &&
            !e.draw.texture_bindings_complete &&
            e.draw.acquisition == rex::graphics::diagnostic::DrawCaptureAvailability::kPartial &&
            e.draw.acquisition_reason == DrawCaptureReason::kTextureUnresolved &&
            e.acquisition == rex::graphics::diagnostic::DrawCaptureAvailability::kPartial &&
            e.acquisition_reason == DrawCaptureReason::kTextureUnresolved;
        okay &= unavailable;
        ledger << "target occurrence=" << i << " owned_event=" << e.id
               << " texture=" << use.texture_index << " expected=U unavailable=" << unavailable
               << " required=" << use.required << " host_issued=" << e.host_issued << '\n';
        continue;
      }
      if (!use.data_available ||
          use.availability != rex::graphics::diagnostic::DrawCaptureAvailability::kComplete ||
          use.unavailable_reason != DrawCaptureReason::kNone ||
          use.texture_index >= owned->textures.size()) {
        okay = false;
        continue;
      }
      const auto& texture = owned->textures[use.texture_index];
      if (i == 3) {
        okay &= texture.texture_instance_id != report.seed.instance &&
                texture.guest_base_page != (target_address >> 12);
        continue;
      }
      target_indices.push_back(use.texture_index);
      const bool b = i != 0;
      const auto& expected = producer_pixels[b ? 1 : 0];
      const bool pixels = texture.bytes == expected && texture.subresources.size() == 1 &&
                          texture.subresources[0].bytes == expected &&
                          texture.subresources[0].row_size == kSurfaceWidth * 4 &&
                          texture.subresources[0].row_count == kSurfaceHeight &&
                          texture.subresources[0].mip_level == 0 &&
                          texture.subresources[0].array_slice == 0 &&
                          texture.subresources[0].host_width == kSurfaceWidth &&
                          texture.subresources[0].host_height == kSurfaceHeight;
      const bool identity =
          texture.texture_instance_id == report.seed.instance &&
          texture.content_generation == report.seed.generation + (i ? (config.fail ? 2 : 1) : 0) &&
          texture.guest_base_page == (target_address >> 12) &&
          texture.guest_width == kSurfaceWidth && texture.guest_height == kSurfaceHeight &&
          texture.guest_format == 6 && texture.guest_endianness == 0 && texture.guest_tiled &&
          !texture.scaled_resolve;
      okay &= pixels && identity;
      ledger << "target occurrence=" << i << " owned_event=" << e.id
             << " texture=" << use.texture_index << " expected=" << (b ? 'B' : 'A')
             << " pixels=" << pixels << " identity=" << identity
             << " instance=" << texture.texture_instance_id
             << " generation=" << texture.content_generation
             << " sha256=" << capture_io::Hash(texture.bytes) << '\n';
    }
    okay &= count == 1;
  }
  if (config.fail && target_indices.size() == 2) {
    // The rejected generation acquired no canonical entry or readback index.
    okay &= target_indices[0] == 0 && target_indices[1] == 2 && owned->textures.size() == 3;
  } else if (!config.fail && target_indices.size() == 4) {
    okay &= target_indices[0] != target_indices[1];
    if (config.independent)
      okay &= target_indices[1] != target_indices[2] && target_indices[2] != target_indices[3] &&
              target_indices[1] != target_indices[3];
    else
      okay &= target_indices[1] == target_indices[2] && target_indices[2] == target_indices[3];
  } else
    okay = false;
  uint32_t use_count = 0;
  std::vector<uint64_t> independent_buffers;
  uint32_t scheduled_copies = 0, rejected_uses = 0, dirty_identities = 0, retry_successes = 0;
  uint64_t seam_generation = 0;
  for (uint32_t i = 0; i < report.count; ++i) {
    const auto& r = report.receipts[i];
    if (r.kind == gpu::Kind::kSeam && r.phase == gpu::Phase::kReloadB) {
      seam_generation = r.texture.generation;
      okay &= seam_generation == report.seed.generation + 1 && r.texture.dirty == 0 && r.a == 1 &&
              r.b == 0;
    }
    if (r.kind == gpu::Kind::kUse) {
      ++use_count;
      okay &= r.a != 0 && r.texture.resource == report.seed.resource &&
              r.texture.generation == report.seed.generation + (config.fail ? 2 : 1) &&
              r.texture.dirty == (config.fail && r.phase != gpu::Phase::kRetryB ? 1 : 0);
    }
    if (r.kind == gpu::Kind::kIdentity &&
        (r.phase == gpu::Phase::kReloadB || r.phase == gpu::Phase::kRepeat) && config.fail) {
      ++dirty_identities;
      okay &= r.texture.instance == report.seed.instance &&
              r.texture.resource == report.seed.resource &&
              r.texture.generation == report.seed.generation + 2 && r.texture.dirty == 1;
    }
    if (r.kind == gpu::Kind::kRejected) {
      ++rejected_uses;
      okay &= config.fail && (r.phase == gpu::Phase::kReloadB || r.phase == gpu::Phase::kRepeat) &&
              r.texture.instance == report.seed.instance &&
              r.texture.resource == report.seed.resource &&
              r.texture.generation == report.seed.generation + 2 && r.texture.dirty == 1 &&
              r.a == 1 && r.b == 1 && r.d != 0;
    }
    if (r.kind == gpu::Kind::kSchedule && r.c) {
      ++scheduled_copies;
      okay &= r.c == kSurfaceBytes && r.b != 0 && r.d == 0;
      independent_buffers.push_back(r.b);
    }
    if (r.kind == gpu::Kind::kEntry && r.phase == gpu::Phase::kRetryB)
      okay &= config.fail && r.texture.generation == seam_generation + 1;
    if (r.kind == gpu::Kind::kSuccess && r.phase == gpu::Phase::kRetryB) {
      ++retry_successes;
      okay &= config.fail && r.texture.instance == report.seed.instance &&
              r.texture.resource == report.seed.resource &&
              r.texture.generation == report.seed.generation + 2 && r.texture.dirty == 0;
    }
  }
  okay &= use_count == (config.fail ? 1 : 3) &&
          scheduled_copies == (!config.fail && config.independent ? 3 : 1) &&
          rejected_uses == (config.fail ? 2 : 0) && dirty_identities == (config.fail ? 2 : 0) &&
          retry_successes == config.fail;
  if (!config.fail && config.independent && independent_buffers.size() == 3)
    okay &= independent_buffers[0] != independent_buffers[1] &&
            independent_buffers[0] != independent_buffers[2] &&
            independent_buffers[1] != independent_buffers[2];
  ledger << "assertions_passed=" << okay << '\n';
  ledger.flush();
  std::cout << "fixture fail=" << config.fail << " independent=" << config.independent
            << " controller_error=" << report.error << " attempts=" << report.post_seed_attempts
            << " selected=" << report.selected << " target_copies=" << target_copies
            << " draws=" << draw_count << " assertion_result=" << okay << '\n';
  return okay && ledger.good();
}
}  // namespace test_fixture

#include "run.inl"
