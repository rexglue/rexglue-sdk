#pragma once
#include <windows.h>

namespace capture_io {
namespace diag = rex::graphics::diagnostic;
inline diag::DrawCaptureMailbox::Token token;
inline std::shared_ptr<const diag::DrawCaptureFrameSnapshot> owned;
inline bool saved = false;
inline std::filesystem::path root;
inline std::string Hash(std::span<const uint8_t> bytes) {
  return rex::crypto::sha256(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
inline bool Bytes(std::string_view name, std::span<const uint8_t> bytes) {
  std::ofstream out(root / name, std::ios::binary | std::ios::noreplace);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}
inline void Arm(const diag::DrawCaptureFrameInputCopyView& copy) {
  if (!token && copy.original_succeeded && copy.copy_mode == "resolve_readback" &&
      copy.written_length) {
    token = diag::GetDrawCaptureMailbox()->StartFrame();
  }
}
inline bool Initialize() {
  const char* value = std::getenv("REREVVED_GPU_FIXTURE_OUTPUT");
  if (!value)
    return false;
  root = std::filesystem::path(value);
  return root.is_absolute() && std::filesystem::create_directory(root);
}
inline bool SaveReceipt(const rex::graphics::d3d12::fixture::TraceReport& report) {
  std::ofstream out(root / "receipt.txt", std::ios::noreplace);
  out << "schema=2 enabled=" << report.enabled << " closed=" << report.closed
      << " incomplete=" << report.incomplete << " count=" << report.count << " lost=" << report.lost
      << " first_lost=" << report.first_lost << '\n';
  for (uint32_t i = 0; i < report.count; ++i) {
    const auto& row = report.rows[i];
    const auto& value = row.texture;
    out << "row=" << row.sequence << " kind=" << uint32_t(row.kind)
        << " phase=" << uint32_t(row.phase) << " cp=" << row.cp << " cache=" << row.cache
        << " shared=" << row.shared << " occurrence=" << row.occurrence << " owned=" << row.owned
        << " observer=" << row.observer << " token=" << row.token << " frame=" << row.frame
        << " submission=" << row.submission << " instance=" << value.instance
        << " generation=" << value.generation << " generation_after=" << row.generation_after
        << " entry_generation=" << row.entry_generation << " resource=" << value.resource
        << " dirty=" << value.dirty << " key=";
    for (uint32_t k = 0; k < value.key_size; ++k)
      out << std::hex << std::setw(2) << std::setfill('0') << uint32_t(value.key[k]);
    out << std::dec << " base=" << value.base_address << ',' << value.base_size
        << " mips=" << value.mip_address << ',' << value.mip_size << " a=" << row.a
        << " b=" << row.b << " c=" << row.c << " d=" << row.d << '\n';
  }
  out.flush();
  return out.good();
}
inline void CopyReceipt(const diag::DrawCaptureFrameInputCopyView& copy) {
  std::ofstream out(root / ("copy-observer-" + std::to_string(copy.id) + ".txt"),
                    std::ios::noreplace);
  out << "observer_id=" << copy.id << " frame=" << copy.frame << " submission=" << copy.submission
      << " mode=" << copy.copy_mode << " succeeded=" << copy.original_succeeded
      << " written_address=" << copy.written_address << " written_length=" << copy.written_length
      << " planned_address=" << copy.planned_address << " planned_length=" << copy.planned_length
      << " rect_present=" << copy.source_rect_present << " rect=" << copy.source_x << ','
      << copy.source_y << ',' << copy.source_width << ',' << copy.source_height
      << " dest_offset=" << copy.copy_dest_offset_x << ',' << copy.copy_dest_offset_y << '\n';
  for (size_t r = 0; r < copy.registers.size(); ++r)
    out << "register=" << r << " value=" << copy.registers[r] << '\n';
}
inline void Take() {
  if (!token)
    return;
  const auto mailbox = diag::GetDrawCaptureMailbox();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!(owned = mailbox->TryTakeFrame(token)) && mailbox->IsPending(token) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
inline bool Raw(std::ostream& out, const std::string& name, std::span<const uint8_t> bytes) {
  out << "payload=" << name << " bytes=" << bytes.size() << " sha256=" << Hash(bytes) << '\n';
  return Bytes(name, bytes);
}
inline bool Words(std::ostream& out, const std::string& name, std::span<const uint32_t> words) {
  return Raw(out, name, {reinterpret_cast<const uint8_t*>(words.data()), words.size_bytes()});
}
inline bool SaveDraw(std::ostream& out, const diag::DrawCaptureFrameEvent& e) {
  const auto& d = e.draw.draw;
  out << "draw event=" << e.id << " frame=" << d.frame << " submission=" << d.submission
      << " vertex_shader_hash=" << d.vertex_shader_hash
      << " pixel_shader_hash=" << d.pixel_shader_hash << " primitive_type=" << d.primitive_type
      << " requested_index_count=" << d.requested_index_count
      << " major_mode_explicit=" << d.major_mode_explicit << " indexed=" << d.indexed
      << " guest_draw_vertex_count=" << d.guest_draw_vertex_count
      << " host_draw_vertex_count=" << d.host_draw_vertex_count
      << " guest_primitive_type=" << d.guest_primitive_type
      << " host_primitive_type=" << d.host_primitive_type
      << " host_vertex_shader_type=" << d.host_vertex_shader_type
      << " tessellation_mode=" << d.tessellation_mode << " guest_index_base=" << d.guest_index_base
      << " guest_index_size=" << d.guest_index_size << " host_index_format=" << d.host_index_format
      << " host_shader_index_endian=" << d.host_shader_index_endian
      << " host_primitive_reset_enabled=" << d.host_primitive_reset_enabled
      << " color_target_written=" << d.color_target_written
      << " used_texture_mask=" << d.used_texture_mask
      << " normalized_color_mask=" << d.normalized_color_mask
      << " native_topology=" << d.native_topology
      << " native_vertex_count=" << d.native_vertex_count
      << " native_index_count=" << d.native_index_count
      << " native_instance_count=" << d.native_instance_count
      << " native_start_vertex=" << d.native_start_vertex
      << " native_start_index=" << d.native_start_index
      << " native_base_vertex=" << d.native_base_vertex
      << " native_start_instance=" << d.native_start_instance
      << " half_pixel_offset=" << d.half_pixel_offset << '\n';
  const auto prefix = "event-" + std::to_string(e.id);
  bool okay = Words(out, prefix + "-registers.bin", d.registers);
  okay &= Words(out, prefix + "-vertex-ucode.bin", d.vertex_ucode);
  okay &= Words(out, prefix + "-pixel-ucode.bin", d.pixel_ucode);
  okay &= Raw(out, prefix + "-edram-before.bin", d.edram_before);
  okay &= Raw(out, prefix + "-edram-after.bin", d.edram_after);
  for (size_t i = 0; i < d.vertex_fetches.size(); ++i) {
    const auto& v = d.vertex_fetches[i];
    out << "vertex event=" << e.id << " fetch=" << v.fetch_constant << " base=" << v.base
        << " availability=" << uint32_t(v.availability)
        << " reason=" << uint32_t(v.unavailable_reason) << " shared=" << bool(v.shared_bytes)
        << '\n';
    const auto name = prefix + "-vertex-" + std::to_string(i);
    okay &= Raw(out, name + "-bytes.bin", v.bytes);
    okay &= Raw(out, name + "-payload.bin", v.Payload());
  }
  const auto& index = d.index;
  out << "index event=" << e.id << " present=" << index.present << " base=" << index.guest_base
      << " format=" << index.format << " endian=" << index.endianness << " count=" << index.count
      << " dma_count=" << index.dma_count << " dma_length=" << index.dma_length
      << " availability=" << uint32_t(index.availability)
      << " reason=" << uint32_t(index.unavailable_reason) << '\n';
  okay &= Raw(out, prefix + "-index.bin", index.bytes);
  return okay;
}
inline bool Save() {
  if (!owned)
    return false;
  std::ofstream out(root / "owned-frame.txt", std::ios::noreplace);
  out << "schema=" << owned->schema_version << " start_frame=" << owned->start_frame
      << " start_submission=" << owned->start_submission << " end_frame=" << owned->end_frame
      << " end_submission=" << owned->end_submission
      << " acquisition=" << uint32_t(owned->acquisition)
      << " reason=" << uint32_t(owned->acquisition_reason) << '\n';
  bool okay = owned->final_swap.source_availability == diag::DrawCaptureAvailability::kComplete &&
              owned->final_swap.output_availability == diag::DrawCaptureAvailability::kComplete &&
              !owned->final_swap.source_bytes.empty() && !owned->final_swap.output_bytes.empty();
  for (const auto& e : owned->events) {
    out << "event=" << e.id << " frame=" << e.frame << " submission=" << e.submission
        << " kind=" << uint32_t(e.kind) << " copy_mode=" << e.copy_mode
        << " frontbuffer=" << e.frontbuffer_ptr << " size=" << e.frontbuffer_width << ","
        << e.frontbuffer_height << " success=" << e.success << " host_issued=" << e.host_issued
        << " execution=" << uint32_t(e.execution_status)
        << " execution_reason=" << uint32_t(e.execution_reason)
        << " acquisition=" << uint32_t(e.acquisition)
        << " acquisition_reason=" << uint32_t(e.acquisition_reason)
        << " draw_acquisition=" << uint32_t(e.draw.acquisition)
        << " draw_reason=" << uint32_t(e.draw.acquisition_reason)
        << " bindings_complete=" << e.draw.texture_bindings_complete
        << " vs=" << e.draw.guest_vertex_shader_hash << " ps=" << e.draw.guest_pixel_shader_hash
        << '\n';
    for (const auto& u : e.draw.texture_uses) {
      out << "use event=" << e.id << " stage=" << u.stage << " binding=" << u.shader_binding_index
          << " fetch=" << u.fetch_constant << " dimension=" << u.dimension
          << " texture=" << u.texture_index << " required=" << u.required
          << " available=" << u.data_available << " acquisition=" << uint32_t(u.availability)
          << " reason=" << uint32_t(u.unavailable_reason) << " sampler=" << u.sampler_value
          << " mapping=" << u.component_mapping << " swizzle=" << u.host_swizzle
          << " signs=" << uint32_t(u.swizzled_signs) << " signed=" << u.is_signed << '\n';
    }
    okay &= SaveDraw(out, e);
  }
  for (size_t i = 0; i < owned->textures.size(); ++i) {
    const auto& t = owned->textures[i];
    out << "texture=" << i << " instance=" << t.texture_instance_id
        << " generation=" << t.content_generation << " base_page=" << t.guest_base_page
        << " mip_page=" << t.guest_mip_page << " guest_size=" << t.guest_width << ','
        << t.guest_height << ',' << t.guest_depth_or_array_size << " pitch=" << t.guest_pitch
        << " guest_format=" << t.guest_format << " endian=" << t.guest_endianness
        << " tiled=" << t.guest_tiled << " packed_mips=" << t.guest_packed_mips
        << " scaled=" << t.scaled_resolve << " host_format=" << t.host_format
        << " host_size=" << t.host_width << ',' << t.host_height << ','
        << t.host_depth_or_array_size << " acquisition=" << uint32_t(t.availability)
        << " reason=" << uint32_t(t.unavailable_reason) << '\n';
    out << "texture_layout=" << i << " mip=" << t.mip_level << " row_pitch=" << t.row_pitch
        << " row_size=" << t.row_size << " rows=" << t.row_count
        << " host_mips=" << t.host_mip_levels << '\n';
    okay &= Raw(out, "texture-" + std::to_string(i) + "-top.bin", t.bytes);
    for (const auto& s : t.subresources) {
      const auto name = "texture-" + std::to_string(i) + "-mip-" + std::to_string(s.mip_level) +
                        "-slice-" + std::to_string(s.array_slice) + ".bin";
      okay &= Bytes(name, s.bytes);
      out << "subresource texture=" << i << " mip=" << s.mip_level << " slice=" << s.array_slice
          << " size=" << s.host_width << ',' << s.host_height << " row_pitch=" << s.row_pitch
          << " row_size=" << s.row_size << " rows=" << s.row_count << " bytes=" << s.bytes.size()
          << " sha256=" << Hash(s.bytes) << " file=" << name << '\n';
    }
  }
  for (const wchar_t* name : {L"rexruntime.dll", L"rexgpu-xenos.dll"}) {
    wchar_t path[32768]{};
    const HMODULE module = GetModuleHandleW(name);
    const DWORD length = module ? GetModuleFileNameW(module, path, 32768) : 0;
    if (!module || !length || length >= 32768) {
      out << "module_provenance_unavailable=true\n";
      okay = false;
      continue;
    }
    out << "module=" << std::filesystem::path(path).string() << '\n';
  }
  okay &= Bytes("final-source.bin", owned->final_swap.source_bytes);
  okay &= Bytes("final-output.bin", owned->final_swap.output_bytes);
  out << "final_source_sha256=" << Hash(owned->final_swap.source_bytes)
      << " final_output_sha256=" << Hash(owned->final_swap.output_bytes)
      << " final_source_availability=" << uint32_t(owned->final_swap.source_availability)
      << " final_output_availability=" << uint32_t(owned->final_swap.output_availability) << '\n';
  const auto& final = owned->final_swap;
  out << "final_source_format=" << final.source_format << " source_size=" << final.source_width
      << ',' << final.source_height << " source_row_pitch=" << final.source_row_pitch
      << " source_row_size=" << final.source_row_size << " source_rows=" << final.source_row_count
      << " final_output_format=" << final.output_format << " output_size=" << final.output_width
      << ',' << final.output_height << " output_row_pitch=" << final.output_row_pitch
      << " output_row_size=" << final.output_row_size << " output_rows=" << final.output_row_count
      << " gamma_pwl=" << final.gamma.use_pwl_gamma_ramp << " gamma_fxaa=" << final.gamma.use_fxaa
      << " gamma_is8bpc=" << final.gamma.is_8bpc << '\n';
  okay &= Words(out, "gamma-256.bin", final.gamma.gamma_256);
  okay &= Words(out, "gamma-pwl.bin", final.gamma.gamma_pwl_rgb);
  out << "gamma_source_format=" << final.gamma.source_format
      << " gamma_output_format=" << final.gamma.output_format
      << " gamma_output_size=" << final.gamma.output_width << ',' << final.gamma.output_height
      << " final_frontbuffer=" << final.frontbuffer_ptr
      << " source_reason=" << uint32_t(final.source_unavailable_reason)
      << " output_reason=" << uint32_t(final.output_unavailable_reason) << '\n';
  out.flush();
  return okay && out.good();
}
}  // namespace capture_io
