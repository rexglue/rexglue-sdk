/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/perf/frame_recorder.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/frame_dump.h>
#include <rex/graphics/frame_events.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/pgo.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");

REXCVAR_DEFINE_INT32(frame_limit, 0, "GPU",
                     "Host guest-frame swap limit in FPS (0 = unlimited). "
                     "VSync and guest timing may impose a lower rate.")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wait_reg_mem_poll_us, 200, "GPU",
                     "Longest sleep, in microseconds, between polls while a WAIT_REG_MEM "
                     "packet waits with vsync on. The guest's wait hint is whole "
                     "milliseconds, which made the GPU command thread oversleep the counter "
                     "it waits for (the vblank counter ticks every 1 ms when the guest "
                     "frame rate is unlocked). 0 = sleep the full hint.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(log_perf_file, false, "GPU",
                    "With log_guest_fps, also write the 5-second summaries, with CPU time per "
                    "thread and CPU/GPU clocks, to perf_log.txt next to the executable "
                    "(Linux; replaced at every launch)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(log_perf_profile, false, "GPU",
                    "With log_perf_file, sample the GPU command thread and the main game thread "
                    "(1 sample per ms of their CPU time) and add their busiest functions to "
                    "perf_log.txt every 30 s (Linux; needs a restart to turn on)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(log_frame_dump, false, "GPU",
                    "Every 20 s, append what one guest frame does with render targets "
                    "(bindings, ownership transfers, resolves, texture loads of GPU-written "
                    "data, draws in between) to frame_dump.txt next to the executable, up to 40 "
                    "frames per launch")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",
                    "Log a guest frame rate summary every 5 seconds: average fps, the "
                    "slowest frame and how many frames took longer than 33 ms (stutters). "
                    "Measured at the guest's frame swap, so unaffected by host presentation.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(frame_recorder, 0, "GPU",
                     "Per-frame time breakdown kept in memory (rex/perf/frame_recorder.h): "
                     "0 = off, 1 = frame sections, 2 = also the sections inside each draw, "
                     "which costs more.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(clear_memory_page_state, true, "GPU",
                    "Refresh page-valid state from GPU-written memory at frame end. "
                    "Disable for minor CPU overhead reduction, but may break memory coherency.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, true, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale. With the D3D9-style "
                    "half-pixel offset, the content of each guest pixel starts at host pixel "
                    "(scale/2, scale/2) of its block, so the block's top-left host pixel "
                    "belongs to the previous guest pixel, and the first row and column of "
                    "the resolve read the uncovered edge (a line along the top of the "
                    "screen at 2x in Fable II)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve_force_addresses, "", "GPU",
                      "Comma-separated guest base addresses whose render-to-texture resolve is "
                      "force-read back to guest memory even when readback_resolve is disabled. "
                      "Each entry (hex/decimal) matches [addr, addr + 1 MiB). E.g. Fable II's "
                      "hero/dog face texture at 0x12704000 (approach from just-harry's "
                      "unofficial Xenia femtofork for Fable II).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport, true, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast_wait, true, "GPU",
                    "With readback_memexport_fast, wait for the previous memexport copy when "
                    "the GPU has not finished it yet, instead of reading the current draw. "
                    "The guest then always sees the data from one memexport draw back, at any "
                    "frame rate, rather than alternating between the previous and the current "
                    "draw depending on how far the GPU is behind.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_shader_compilation, true, "GPU",
                    "Compile shaders and create pipelines asynchronously in background "
                    "threads. This reduces stutter but may cause brief visual artifacts while "
                    "pipelines are being prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_pipeline_wait, true, "GPU",
                    "When a draw needs a pipeline that is still being created in the background, "
                    "wait for it instead of skipping the draw. Skipped draws cause missing "
                    "objects and corrupted textures, at the cost of a short stall the first time "
                    "a pipeline is needed. Only used with async_shader_compilation.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Shared by the backends that implement it (Vulkan here; upstream main defines
// the same cvar in the D3D12 command processor - keep a single definition when
// merging).
REXCVAR_DEFINE_BOOL(gpu_sync_at_frame_start, false, "GPU",
                    "At the start of each frame, wait until the GPU has finished all earlier "
                    "work, so the GPU is never more than one frame behind")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(async_pipeline_wait_timeout_ms, 5000, "GPU",
                     "Longest time, in milliseconds, to wait for one pipeline with "
                     "async_pipeline_wait before skipping the draw anyway.")
    .range(100, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::HasNonDefaultValue("readback_resolve");
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::ShouldForceReadbackResolve(uint32_t base) const {
  const std::string& spec = REXCVAR_GET(readback_resolve_force_addresses);
  if (spec.empty()) {
    return false;
  }

  // The hero/dog EDRAM-resolve copy is issued as a PM4_DRAW_INDX_2 draw (see the
  // `may_require_readback_resolve_` set in the draw opcode dispatch). Regular
  // PM4_DRAW_INDX draws can also be a kRectangleList/3 copy to the same
  // destination (the game sampling that texture mid-frame); reading those back
  // captures a mid-update state and makes the dog look red / shifting.
  if (!may_require_readback_resolve_) {
    return false;
  }

  // Only the full-texture blit should force a readback, matching the femtofork's
  // detection: a fullscreen kRectangleList draw with 3 indices that copies to the
  // texture base. The dog's texture is touched by extra / partial copies near the
  // same region; reading those back mid-update makes it look glitchy / shifting.
  const reg::VGT_DRAW_INITIATOR vgt = register_file_->Get<reg::VGT_DRAW_INITIATOR>();
  if (vgt.prim_type != xenos::PrimitiveType::kRectangleList || vgt.num_indices != 0x03) {
    return false;
  }

  // Each entry is a guest base address (hex with an optional 0x prefix, or
  // decimal); it matches [addr, addr + kForceReadbackWindow). The window must be
  // small: 0x12704000 sits in a densely-packed region with other EDRAM resolve
  // targets (e.g. 0x12724000) blitted in rapid bursts. A wide window forces an
  // immediate GPU-sync readback for each, stalling the GPU mid-frame (the dog
  // looked glitchy / red / shifting). 64 KiB covers the target while the nearest
  // unrelated target is 128 KiB away.
  constexpr uint32_t kForceReadbackWindow = 1u << 16;  // 64 KiB
  size_t start = 0;
  while (start < spec.size()) {
    size_t end = spec.find(',', start);
    if (end == std::string::npos) {
      end = spec.size();
    }
    std::string_view token = spec.substr(start, end - start);
    // Trim surrounding whitespace.
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
      token.remove_prefix(1);
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
      token.remove_suffix(1);
    }
    if (!token.empty()) {
      uint64_t addr = 0;
      try {
        size_t pos = 0;
        addr = std::stoull(std::string(token), &pos, 0);  // base 0: 0x-hex or decimal
      } catch (const std::exception&) {
        addr = 0;  // Malformed token - ignore.
      }
      uint32_t a = static_cast<uint32_t>(addr);
      if (base >= a && base - a < kForceReadbackWindow) {
        // Throttled diagnostic: confirm the forced readback is triggering and on the
        // expected address (helps discover a second address for the dog).
        static int s_forced_readback_log = 0;
        if (s_forced_readback_log < 24) {
          REXGPU_INFO("Forced readback-resolve for RB_COPY_DEST_BASE={:#010x} "
                      "(matched force address {:#010x})",
                      base, a);
          ++s_forced_readback_log;
        }
        return true;
      }
    }
    start = end + 1;
  }
  return false;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  // Called for every draw. HasNonDefaultValue takes the cvar registry mutex and
  // does a string-keyed lookup, so only re-query it now and then; the cvars are
  // hot-reloadable, and a change still takes effect within a frame or two.
  constexpr uint32_t kLegacyOverrideRefreshInterval = 1024;
  static thread_local uint32_t refresh_countdown = 0;
  static thread_local bool legacy_override = false;
  if (refresh_countdown == 0) {
    refresh_countdown = kLegacyOverrideRefreshInterval;
    legacy_override = legacy_readback_memexport_cvar_name_ &&
                      rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_);
  }
  --refresh_countdown;
  if (legacy_override) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      REX_FRAME_ZONE(kIdle);
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
          OnWaitTick();
        }

        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
#ifndef NDEBUG
  // Diagnostic only, and the lookup is a several-thousand-case switch on the
  // hottest path of the command processor, so keep it out of release builds.
  if (!regs.GetRegisterInfo(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }
#endif

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {}

void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  OnPrimaryBufferEnd();

  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  ++work_stats_.packets;
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
  if (packet == 0) {
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  if (!write_one_reg) {
    // Sequential registers: hand the whole run to the backend, which handles
    // runs of shader constants in bulk instead of one virtual call each.
    WriteRegisterRangeFromRing(reader, base_index, count);
    return true;
  }
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    WriteRegister(base_index, reg_data);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

// --- Host performance sampling for the log_guest_fps summary (Linux) ---------
// Every 5 s: CPU time per thread (busiest first), CPU clocks, and the GPU's
// average utilisation and current clock. Also written, with the frame rate
// line, to perf_log.txt next to the executable (log_perf_file), a short file
// meant to be shared when reporting performance. Runs on the command
// processor thread; reading /proc and sysfs a few times per second is cheap.
#if defined(__linux__)
namespace {

std::string ReadSmallFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  char buf[4096];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  return std::string(buf, n);
}

// --- Built-in sampling profiler (perf_log.txt) --------------------------------
// A per-thread CPU-time timer (SIGPROF, 1 ms of that thread's CPU time) on the
// GPU command thread and the main game thread records the interrupted
// instruction address into a ring buffer. Every 30 s the addresses are mapped
// to function names (from the modules' ELF symbol tables, which the packaged
// binaries keep) and the top functions per thread are written to the log.
constexpr int kProfThreads = 4;  // GPU Commands, GameThread, 3D Engine, GPU Submit
constexpr uint32_t kProfRing = 1u << 16;
const char* const kProfThreadPrefix[kProfThreads] = {"GPU Commands", "GameThread", "3D Engine",
                                                    "GPU Submit"};

struct ProfRing {
  std::atomic<uint32_t> head{0};
  uintptr_t pc[kProfRing];
};
ProfRing g_prof_rings[kProfThreads];

void ProfSignalHandler(int, siginfo_t* info, void* uc_void) {
  const int idx = info->si_value.sival_int;
  if (idx < 0 || idx >= kProfThreads) return;
  const auto* uc = static_cast<const ucontext_t*>(uc_void);
#if defined(__x86_64__)
  const uintptr_t pc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  const uintptr_t pc = uintptr_t(uc->uc_mcontext.pc);
#else
  const uintptr_t pc = 0;
#endif
  ProfRing& r = g_prof_rings[idx];
  const uint32_t at = r.head.fetch_add(1, std::memory_order_relaxed);
  r.pc[at & (kProfRing - 1)] = pc;
}

// Function symbols of one loaded module, from its ELF .symtab (or .dynsym).
struct ModuleSymbols {
  struct Sym {
    uintptr_t start, size;
    std::string name;
  };
  std::vector<Sym> syms;  // sorted by start (offsets from the load base)
  const Sym* Find(uintptr_t off) const {
    auto it = std::upper_bound(syms.begin(), syms.end(), off,
                               [](uintptr_t v, const Sym& s) { return v < s.start; });
    if (it == syms.begin()) return nullptr;
    --it;
    return off < it->start + std::max<uintptr_t>(it->size, 1) ? &*it : nullptr;
  }
};

ModuleSymbols LoadModuleSymbols(const char* path) {
  ModuleSymbols out;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  std::vector<uint8_t> data;
  std::fseek(f, 0, SEEK_END);
  data.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  const bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  if (!ok || data.size() < sizeof(Elf64_Ehdr)) return out;
  const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(data.data());
  if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
      eh->e_shoff + uint64_t(eh->e_shnum) * sizeof(Elf64_Shdr) > data.size()) {
    return out;
  }
  const auto* sh = reinterpret_cast<const Elf64_Shdr*>(data.data() + eh->e_shoff);
  for (int pass = 0; pass < 2 && out.syms.empty(); ++pass) {
    const uint32_t want = pass == 0 ? SHT_SYMTAB : SHT_DYNSYM;
    for (int i = 0; i < eh->e_shnum; ++i) {
      if (sh[i].sh_type != want || sh[i].sh_link >= eh->e_shnum) continue;
      const Elf64_Shdr& strs = sh[sh[i].sh_link];
      if (sh[i].sh_offset + sh[i].sh_size > data.size() ||
          strs.sh_offset + strs.sh_size > data.size()) {
        continue;
      }
      const auto* sym = reinterpret_cast<const Elf64_Sym*>(data.data() + sh[i].sh_offset);
      const size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
      for (size_t k = 0; k < n; ++k) {
        if (ELF64_ST_TYPE(sym[k].st_info) != STT_FUNC || sym[k].st_value == 0 ||
            sym[k].st_name >= strs.sh_size) {
          continue;
        }
        out.syms.push_back({uintptr_t(sym[k].st_value), uintptr_t(sym[k].st_size),
                            reinterpret_cast<const char*>(data.data() + strs.sh_offset +
                                                          sym[k].st_name)});
      }
    }
  }
  std::sort(out.syms.begin(), out.syms.end(),
            [](const ModuleSymbols::Sym& a, const ModuleSymbols::Sym& b) {
              return a.start < b.start;
            });
  return out;
}

// Address range of one loaded module, from dl_iterate_phdr. Sampled addresses
// are mapped to modules with these instead of dladdr, which walks a module's
// whole dynamic symbol table on every call: for the game executable that made
// each 30 s report stall the GPU command thread for 50-140 ms.
struct LoadedModule {
  uintptr_t lo, hi;  // [lo, hi) covering the PT_LOAD segments
  uintptr_t bias;    // added to symbol values (dlpi_addr)
  std::string path;  // "" for the main program
};

int CollectLoadedModule(dl_phdr_info* info, size_t, void* data) {
  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    lo = std::min(lo, uintptr_t(info->dlpi_addr + ph.p_vaddr));
    hi = std::max(hi, uintptr_t(info->dlpi_addr + ph.p_vaddr + ph.p_memsz));
  }
  if (lo < hi) {
    static_cast<std::vector<LoadedModule>*>(data)->push_back(
        {lo, hi, uintptr_t(info->dlpi_addr), info->dlpi_name ? info->dlpi_name : ""});
  }
  return 0;
}

std::string ShortFunctionName(const std::string& mangled) {
  int status = 0;
  char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
  std::string name = status == 0 && demangled ? demangled : mangled;
  std::free(demangled);
  // Drop "(anonymous namespace)::" (its parentheses would cut the name short)
  // and the parameter list, to keep log lines readable.
  for (size_t at; (at = name.find("(anonymous namespace)::")) != std::string::npos;) {
    name.erase(at, sizeof("(anonymous namespace)::") - 1);
  }
  if (const size_t paren = name.find('('); paren != std::string::npos) name.resize(paren);
  if (name.size() > 60) name = "..." + name.substr(name.size() - 57);
  return name;
}

struct HostPerfState {
  int gpu_busy_fd = -2;  // -2 = not opened yet, -1 = unavailable
  std::string gpu_dir;   // /sys/class/drm/cardN/device
  uint64_t gpu_busy_sum = 0;
  uint32_t gpu_busy_samples = 0;
  // amdgpu hwmon power1_average (microwatts; the whole APU on the Steam Deck).
  int power_fd = -1;
  uint64_t power_sum = 0;
  uint32_t power_samples = 0;
  std::unordered_map<int, uint64_t> prev_ticks;  // tid -> utime + stime
  std::FILE* file = nullptr;
  bool file_failed = false;
  // Profiler.
  bool prof_handler_installed = false;
  int prof_tid[kProfThreads] = {};
  uint32_t prof_consumed[kProfThreads] = {};
  uint32_t prof_reports = 0;
  std::unordered_map<std::string, ModuleSymbols> prof_modules;
  std::unordered_map<uintptr_t, std::string> prof_pc_names;
  std::vector<LoadedModule> prof_loaded;
  bool prof_loaded_refreshed = false;  // re-listed during the current report
  rex::memory::ProtectStats protect_reported = {};
  uint64_t texture_loads_reported = 0;
  uint64_t upload_bytes_reported = 0;
  uint64_t texture_loads_skipped_reported = 0;
  uint64_t upload_bytes_skipped_reported = 0;
  // EDRAM emulation counters of the "EDRAM per frame" line.
  uint64_t gpu_emulation_reported[16] = {};
  uint64_t occlusion_reported[6] = {};
};

// Starts the per-thread profiling timers once the threads exist.
void ProfEnsureTimers(HostPerfState& s) {
  if (!REXCVAR_GET(log_perf_profile)) return;
  if (!s.prof_handler_installed) {
    struct sigaction sa = {};
    sa.sa_sigaction = ProfSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, nullptr) != 0) return;
    s.prof_handler_installed = true;
  }
  DIR* dir = ::opendir("/proc/self/task");
  if (!dir) return;
  while (const dirent* e = ::readdir(dir)) {
    const int tid = std::atoi(e->d_name);
    if (tid <= 0) continue;
    const std::string comm = ReadSmallFile(fmt::format("/proc/self/task/{}/comm", tid));
    for (int i = 0; i < kProfThreads; ++i) {
      if (s.prof_tid[i] != 0 || comm.rfind(kProfThreadPrefix[i], 0) != 0) continue;
      // The CPU-time clock of another thread (MAKE_THREAD_CPUCLOCK, SCHED).
      const clockid_t clock = clockid_t((~unsigned(tid)) << 3) | 6;
      struct sigevent sev = {};
      sev.sigev_notify = SIGEV_THREAD_ID;
      sev.sigev_signo = SIGPROF;
      sev.sigev_value.sival_int = i;
      sev._sigev_un._tid = tid;
      timer_t timer;
      if (timer_create(clock, &sev, &timer) != 0) continue;
      struct itimerspec spec = {};
      spec.it_interval.tv_nsec = 1000000;  // every 1 ms of that thread's CPU time
      spec.it_value.tv_nsec = 1000000;
      if (timer_settime(timer, 0, &spec, nullptr) != 0) {
        timer_delete(timer);
        continue;
      }
      s.prof_tid[i] = tid;
      s.prof_consumed[i] = g_prof_rings[i].head.load(std::memory_order_relaxed);
    }
  }
  ::closedir(dir);
}

// The loaded module containing pc, or nullptr. The module list is read again
// (once per report) when pc is outside every known module.
const LoadedModule* ProfFindModule(HostPerfState& s, uintptr_t pc) {
  auto find = [&]() -> const LoadedModule* {
    for (const LoadedModule& m : s.prof_loaded) {
      if (pc >= m.lo && pc < m.hi) return &m;
    }
    return nullptr;
  };
  const LoadedModule* m = find();
  if (!m && !s.prof_loaded_refreshed) {
    s.prof_loaded_refreshed = true;
    s.prof_loaded.clear();
    dl_iterate_phdr(CollectLoadedModule, &s.prof_loaded);
    m = find();
  }
  return m;
}

// Function name for a sampled PC (mangled; shortened when printed).
std::string ProfResolvePc(HostPerfState& s, uintptr_t pc) {
  const LoadedModule* m = pc ? ProfFindModule(s, pc) : nullptr;
  if (!m) {
    return "[unknown]";
  }
  if (!m->path.empty() && !std::strchr(m->path.c_str(), '/')) {
    // No file to read (the vDSO): its dynamic symbol table is tiny, so dladdr
    // is cheap here.
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname) {
      return info.dli_sname;
    }
    return "[" + m->path + "]";
  }
  auto it = s.prof_modules.find(m->path);
  if (it == s.prof_modules.end()) {
    // The main program has an empty name.
    const char* file = m->path.empty() ? "/proc/self/exe" : m->path.c_str();
    it = s.prof_modules.emplace(m->path, LoadModuleSymbols(file)).first;
  }
  if (const ModuleSymbols::Sym* sym = it->second.Find(pc - m->bias)) {
    return sym->name;
  }
  const char* slash = std::strrchr(m->path.c_str(), '/');
  return std::string("[") + (slash ? slash + 1 : m->path.empty() ? "main" : m->path.c_str()) + "]";
}

// The busiest instruction addresses inside one library, as hex offsets from
// its load base with their sample counts, sorted by offset. Inlining folds most
// of the per-draw code into a few large functions, so the function list alone
// can't say which part of them costs the time; these offsets are mapped to
// source lines afterwards with llvm-symbolizer on the unstripped library of
// the same build.
std::string ProfHotSpots(HostPerfState& s, const std::unordered_map<uintptr_t, uint32_t>& pc_counts,
                         uint32_t total, const char* library) {
  const size_t library_length = std::strlen(library);
  std::vector<std::pair<uintptr_t, uint32_t>> spots;
  uint32_t in_library = 0;
  for (const auto& [pc, count] : pc_counts) {
    const LoadedModule* m = pc ? ProfFindModule(s, pc) : nullptr;
    if (!m || m->path.size() < library_length ||
        m->path.compare(m->path.size() - library_length, library_length, library) != 0) {
      continue;
    }
    in_library += count;
    spots.emplace_back(pc - m->bias, count);
  }
  if (spots.empty()) return {};
  // Keep the busiest addresses (at least 0.05% of the samples each).
  std::sort(spots.begin(), spots.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  const uint32_t min_count = std::max<uint32_t>(2, total / 2000);
  size_t kept = 0;
  while (kept < spots.size() && kept < 300 && spots[kept].second >= min_count) ++kept;
  spots.resize(kept);
  std::sort(spots.begin(), spots.end());
  std::string out = fmt::format("\n         hot spots {} ({} of {} samples, offset:count):",
                                library, in_library, total);
  for (size_t k = 0; k < spots.size(); ++k) {
    out += fmt::format("{}{:x}:{}", k % 12 ? " " : "\n           ", spots[k].first,
                       spots[k].second);
  }
  return out;
}

// Top functions per profiled thread since the last report ("" when nothing).
std::string ProfReport(HostPerfState& s) {
  std::string out;
  s.prof_loaded_refreshed = false;
  for (int i = 0; i < kProfThreads; ++i) {
    if (!s.prof_tid[i]) continue;
    ProfRing& r = g_prof_rings[i];
    const uint32_t head = r.head.load(std::memory_order_acquire);
    uint32_t from = s.prof_consumed[i];
    if (head - from > kProfRing) from = head - kProfRing;
    s.prof_consumed[i] = head;
    const uint32_t total = head - from;
    if (!total) continue;
    // Aggregate by raw PC first, then resolve each unique PC once (cached
    // across reports) - dladdr per sample stalled this thread for ~0.7 s.
    std::unordered_map<uintptr_t, uint32_t> pc_counts;
    pc_counts.reserve(4096);
    for (uint32_t k = from; k != head; ++k) {
      ++pc_counts[r.pc[k & (kProfRing - 1)]];
    }
    std::unordered_map<std::string, uint32_t> counts;
    for (const auto& [pc, count] : pc_counts) {
      auto cached = s.prof_pc_names.find(pc);
      if (cached == s.prof_pc_names.end()) {
        cached = s.prof_pc_names.emplace(pc, ProfResolvePc(s, pc)).first;
      }
      counts[cached->second] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> top(counts.begin(), counts.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    out += fmt::format("\n         profile {} ({} samples, last 30 s):", kProfThreadPrefix[i],
                       total);
    for (size_t k = 0; k < top.size() && k < 25; ++k) {
      out += fmt::format("\n           {:5.1f}%  {}", 100.0 * top[k].second / total,
                         ShortFunctionName(top[k].first));
    }
    if (i == 0) {
      out += ProfHotSpots(s, pc_counts, total, "librexgpu-xenos.so");
    }
  }
  return out;
}
HostPerfState& HostPerf() {
  static HostPerfState state;
  return state;
}

}  // namespace

void CommandProcessor::HostPerfSampleFrame() {
  HostPerfState& s = HostPerf();
  if (s.gpu_busy_fd == -2) {
    s.gpu_busy_fd = -1;
    for (int i = 0; i < 8 && s.gpu_busy_fd < 0; ++i) {
      const std::string dir = fmt::format("/sys/class/drm/card{}/device", i);
      const int fd = ::open((dir + "/gpu_busy_percent").c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0) {
        s.gpu_busy_fd = fd;
        s.gpu_dir = dir;
        for (int h = 0; h < 16 && s.power_fd < 0; ++h) {
          s.power_fd = ::open(fmt::format("{}/hwmon/hwmon{}/power1_average", dir, h).c_str(),
                              O_RDONLY | O_CLOEXEC);
        }
      }
    }
  }
  if (s.power_fd >= 0) {
    char power_buf[24];
    const ssize_t power_n = ::pread(s.power_fd, power_buf, sizeof(power_buf) - 1, 0);
    if (power_n > 0) {
      power_buf[power_n] = 0;
      s.power_sum += uint64_t(std::strtoull(power_buf, nullptr, 10));
      ++s.power_samples;
    }
  }
  if (s.gpu_busy_fd < 0) return;
  char buf[16];
  const ssize_t n = ::pread(s.gpu_busy_fd, buf, sizeof(buf) - 1, 0);
  if (n > 0) {
    buf[n] = 0;
    s.gpu_busy_sum += uint64_t(std::strtoul(buf, nullptr, 10));
    ++s.gpu_busy_samples;
  }
}

std::string CommandProcessor::HostPerfReport(double seconds) {
  HostPerfState& s = HostPerf();
  ProfEnsureTimers(s);
  // Threads: CPU time since the last report, busiest first.
  struct Busy {
    double pct;
    std::string name;
  };
  std::vector<Busy> busy;
  std::unordered_map<int, uint64_t> ticks_now;
  const double hz = double(::sysconf(_SC_CLK_TCK));
  if (DIR* dir = ::opendir("/proc/self/task")) {
    while (const dirent* e = ::readdir(dir)) {
      const int tid = std::atoi(e->d_name);
      if (tid <= 0) continue;
      const std::string stat = ReadSmallFile(fmt::format("/proc/self/task/{}/stat", tid));
      const size_t open = stat.find('('), close = stat.rfind(')');
      if (open == std::string::npos || close == std::string::npos || close < open) continue;
      // Fields after ") ": state(3) ... utime(14) stime(15).
      uint64_t utime = 0, stime = 0;
      int field = 3;
      const char* p = stat.c_str() + close + 2;
      while (*p && field <= 15) {
        const char* end = std::strchr(p, ' ');
        if (field == 14) utime = std::strtoull(p, nullptr, 10);
        if (field == 15) stime = std::strtoull(p, nullptr, 10);
        if (!end) break;
        p = end + 1;
        ++field;
      }
      const uint64_t ticks = utime + stime;
      ticks_now[tid] = ticks;
      const auto prev = s.prev_ticks.find(tid);
      if (prev == s.prev_ticks.end() || ticks <= prev->second) continue;
      const double pct = 100.0 * double(ticks - prev->second) / hz / seconds;
      if (pct >= 1.0) busy.push_back({pct, stat.substr(open + 1, close - open - 1)});
    }
    ::closedir(dir);
  }
  const bool first = s.prev_ticks.empty();
  s.prev_ticks = std::move(ticks_now);
  std::string out;
  if (first) {
    out = "threads: (first sample)";
  } else {
    std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.pct > b.pct; });
    out = "threads % of a core:";
    for (size_t i = 0; i < busy.size() && i < 10; ++i) {
      out += fmt::format("{} {} {:.0f}", i ? " |" : "", busy[i].name, busy[i].pct);
    }
  }
  // CPU clocks.
  uint64_t khz_sum = 0, khz_max = 0, cpus = 0;
  for (int cpu = 0; cpu < 64; ++cpu) {
    const std::string v =
        ReadSmallFile(fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/scaling_cur_freq", cpu));
    if (v.empty()) {
      if (cpu > 0) break;
      continue;
    }
    const uint64_t khz = std::strtoull(v.c_str(), nullptr, 10);
    khz_sum += khz;
    khz_max = std::max(khz_max, khz);
    ++cpus;
  }
  if (cpus) {
    out += fmt::format("; CPU MHz avg {} max {}", khz_sum / cpus / 1000, khz_max / 1000);
  }
  // GPU utilisation (average of per-frame samples) and current shader clock.
  if (s.gpu_busy_samples) {
    out += fmt::format("; GPU busy {}%", s.gpu_busy_sum / s.gpu_busy_samples);
    const std::string sclk = ReadSmallFile(s.gpu_dir + "/pp_dpm_sclk");
    const size_t star = sclk.find('*');
    if (star != std::string::npos) {
      const size_t line_start = sclk.rfind('\n', star);
      const size_t colon = sclk.find(':', line_start == std::string::npos ? 0 : line_start);
      if (colon != std::string::npos && colon < star) {
        out += fmt::format(" at {} MHz", std::strtoul(sclk.c_str() + colon + 1, nullptr, 10));
      }
    }
  }
  s.gpu_busy_sum = 0;
  s.gpu_busy_samples = 0;
  if (s.power_samples) {
    out += fmt::format("; APU {:.1f} W", double(s.power_sum) / double(s.power_samples) / 1e6);
  }
  s.power_sum = 0;
  s.power_samples = 0;

  // Host work per guest frame since the last report.
  const double frames = std::max(1.0, double(fps_log_frames_));
  const WorkStats& w = work_stats_;
  const WorkStats& p = work_stats_reported_;
  auto per_frame = [&](uint64_t now_value, uint64_t then_value) {
    return uint64_t(double(now_value - then_value) / frames + 0.5);
  };
  const rex::memory::ProtectStats protect = rex::memory::GetProtectStats();
  const uint64_t texture_loads = g_frame_events.texture_loads.load(std::memory_order_relaxed);
  const uint64_t upload_bytes = g_frame_events.upload_bytes.load(std::memory_order_relaxed);
  const uint64_t texture_loads_skipped =
      g_frame_events.texture_loads_skipped.load(std::memory_order_relaxed);
  const uint64_t upload_bytes_skipped =
      g_frame_events.upload_bytes_skipped.load(std::memory_order_relaxed);
  out += fmt::format(
      "\n         per frame: {} draws, {} packets, {} pipeline binds, {} render passes, "
      "{:.1f} submissions | texture sets {} written {} reused, constant sets {}, set binds {} | "
      "uploads sys {} vs {} ps {} bool {} fetch {} | mprotect {} ({} KB), watch faults {} | "
      "texture loads {} (+{} unchanged, skipped), {} KB uploaded (+{} KB unchanged, skipped)",
      per_frame(w.draws, p.draws), per_frame(w.packets, p.packets),
      per_frame(w.pipeline_binds, p.pipeline_binds), per_frame(w.render_passes, p.render_passes),
      double(w.submissions - p.submissions) / frames,
      per_frame(w.texture_sets_written, p.texture_sets_written),
      per_frame(w.texture_sets_reused, p.texture_sets_reused),
      per_frame(w.constant_sets_written, p.constant_sets_written),
      per_frame(w.descriptor_set_binds, p.descriptor_set_binds),
      per_frame(w.constant_uploads[0], p.constant_uploads[0]),
      per_frame(w.constant_uploads[1], p.constant_uploads[1]),
      per_frame(w.constant_uploads[2], p.constant_uploads[2]),
      per_frame(w.constant_uploads[3], p.constant_uploads[3]),
      per_frame(w.constant_uploads[4], p.constant_uploads[4]),
      per_frame(protect.protect_calls, s.protect_reported.protect_calls),
      per_frame(protect.protect_bytes, s.protect_reported.protect_bytes) / 1024,
      per_frame(protect.watch_faults, s.protect_reported.watch_faults),
      per_frame(texture_loads, s.texture_loads_reported),
      per_frame(texture_loads_skipped, s.texture_loads_skipped_reported),
      per_frame(upload_bytes, s.upload_bytes_reported) / 1024,
      per_frame(upload_bytes_skipped, s.upload_bytes_skipped_reported) / 1024);
  {
    // Which threads take the write-watch faults, and on which guest pages.
    const rex::memory::WatchFaultDetail faults = rex::memory::TakeWatchFaultDetail();
    if (!faults.threads.empty()) {
      out += "\n         watch faults per frame by thread:";
      for (size_t i = 0; i < faults.threads.size() && i < 6; ++i) {
        std::string name = ReadSmallFile(
            fmt::format("/proc/self/task/{}/comm", faults.threads[i].first));
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
        out += fmt::format("{} {} {:.1f}", i ? "," : "", name.empty() ? "?" : name,
                           double(faults.threads[i].second) / frames);
      }
      out += "; hottest pages:";
      for (size_t i = 0; i < faults.pages.size() && i < 8; ++i) {
        out += fmt::format(" {:08X} {:.1f}", faults.pages[i].first,
                           double(faults.pages[i].second) / frames);
      }
      out += fmt::format(" ({} pages)", faults.pages.size());
    }
  }
  work_stats_reported_ = work_stats_;
  s.protect_reported = protect;
  s.texture_loads_reported = texture_loads;
  s.upload_bytes_reported = upload_bytes;
  s.texture_loads_skipped_reported = texture_loads_skipped;
  s.upload_bytes_skipped_reported = upload_bytes_skipped;

  // EDRAM emulation work: render target ownership transfers, resolves, and
  // texture loads of GPU-written data.
  const uint64_t gpu_emulation[16] = {
      g_frame_events.rt_transfers.load(std::memory_order_relaxed),
      g_frame_events.rt_transfer_tiles.load(std::memory_order_relaxed),
      g_frame_events.rt_transfers_to_depth.load(std::memory_order_relaxed),
      g_frame_events.rt_transfers_skipped.load(std::memory_order_relaxed),
      g_frame_events.resolves.load(std::memory_order_relaxed),
      g_frame_events.resolve_dump_tiles.load(std::memory_order_relaxed),
      g_frame_events.resolve_clears.load(std::memory_order_relaxed),
      g_frame_events.texture_loads_gpu_written.load(std::memory_order_relaxed),
      g_frame_events.resolves_direct.load(std::memory_order_relaxed),
      g_frame_events.resolves_direct_memory_skipped.load(std::memory_order_relaxed),
      g_frame_events.clears_forwarded.load(std::memory_order_relaxed),
      g_frame_events.tile_draws_merged.load(std::memory_order_relaxed),
      g_frame_events.draw_failures.load(std::memory_order_relaxed),
      g_frame_events.missing_textures.load(std::memory_order_relaxed),
      g_frame_events.memexport_draws_skipped.load(std::memory_order_relaxed),
      g_frame_events.memexport_draws_cpu_kept.load(std::memory_order_relaxed)};
  uint64_t gpu_emulation_per_frame[16];
  for (int i = 0; i < 16; ++i) {
    gpu_emulation_per_frame[i] = per_frame(gpu_emulation[i], s.gpu_emulation_reported[i]);
    s.gpu_emulation_reported[i] = gpu_emulation[i];
  }
  out += fmt::format(
      "\n         EDRAM per frame: {} render target copies ({} tiles, {} to depth; +{} "
      "skipped), {} resolves ({} tiles dumped; {} direct to texture, {} of those without the "
      "memory copy), {} resolve clears, {} clears forwarded, {} second-tile draws merged, {} "
      "texture loads of GPU-written data; {} failed draws, {} null-texture fetches; "
      "CPU-computed memexport draws: {} skipped, {} run",
      gpu_emulation_per_frame[0], gpu_emulation_per_frame[1], gpu_emulation_per_frame[2],
      gpu_emulation_per_frame[3], gpu_emulation_per_frame[4], gpu_emulation_per_frame[5],
      gpu_emulation_per_frame[8], gpu_emulation_per_frame[9], gpu_emulation_per_frame[6],
      gpu_emulation_per_frame[10], gpu_emulation_per_frame[11], gpu_emulation_per_frame[7],
      gpu_emulation_per_frame[12], gpu_emulation_per_frame[13], gpu_emulation_per_frame[14],
      gpu_emulation_per_frame[15]);
  {
    const uint64_t occlusion[6] = {
        g_frame_events.occlusion_begun.load(std::memory_order_relaxed),
        g_frame_events.occlusion_measured.load(std::memory_order_relaxed),
        g_frame_events.occlusion_zero.load(std::memory_order_relaxed),
        g_frame_events.occlusion_reported_visible.load(std::memory_order_relaxed),
        g_frame_events.occlusion_split.load(std::memory_order_relaxed),
        g_frame_events.occlusion_unpaired.load(std::memory_order_relaxed)};
    uint64_t occlusion_per_frame[6];
    for (int i = 0; i < 6; ++i) {
      occlusion_per_frame[i] = per_frame(occlusion[i], s.occlusion_reported[i]);
      s.occlusion_reported[i] = occlusion[i];
    }
    if (occlusion_per_frame[0]) {
      out += fmt::format(
          "\n         occlusion queries per frame: {} begun, {} measured ({} with no samples), {} "
          "reported visible unmeasured ({} split by a render pass), {} unpaired",
          occlusion_per_frame[0], occlusion_per_frame[1], occlusion_per_frame[2],
          occlusion_per_frame[3], occlusion_per_frame[4], occlusion_per_frame[5]);
    }
  }

  if (++s.prof_reports % 6 == 0) {  // every 30 s
    out += ProfReport(s);
    // PGO-instrumented builds: save the profile counters of both libraries.
    const bool gpu_profile = rex::pgo::WriteModuleProfile();
    const bool runtime_profile = rex::memory::WriteRuntimeProfile();
    if (gpu_profile || runtime_profile) {
      out += fmt::format("\n         PGO profile data saved (graphics {}, runtime {})",
                         gpu_profile ? "yes" : "no", runtime_profile ? "yes" : "no");
    }
  }
  return out;
}

void CommandProcessor::HostPerfFileWrite(const std::string& fps_line, const std::string& host) {
  HostPerfState& s = HostPerf();
  if (!REXCVAR_GET(log_perf_file) || s.file_failed) return;
  if (!s.file) {
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string path = "perf_log.txt";
    if (n > 0) {
      exe[n] = 0;
      std::string dir(exe);
      dir.resize(dir.rfind('/') + 1);
      path = dir + "perf_log.txt";
    }
    s.file = std::fopen(path.c_str(), "w");
    if (!s.file) {
      s.file_failed = true;
      REXGPU_WARN("[host-perf] cannot write {}", path);
      return;
    }
    const std::time_t t = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::fprintf(s.file,
                 "Fable II performance log, started %s. One entry every 5 seconds: game frame "
                 "rate, then CPU %% per thread (100 = one full core), CPU and GPU clocks.\n",
                 stamp);
  }
  const std::time_t t = std::time(nullptr);
  char stamp[16];
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&t));
  std::fprintf(s.file, "%s %s\n         %s\n", stamp, fps_line.c_str(), host.c_str());
  std::fflush(s.file);
}
#else
void CommandProcessor::HostPerfSampleFrame() {}
std::string CommandProcessor::HostPerfReport(double) { return {}; }
void CommandProcessor::HostPerfFileWrite(const std::string&, const std::string&) {}
#endif

namespace {

// Ends the frame being dumped (writes it out) and decides whether to dump the
// next one. Called at every guest frame swap.
void FrameDumpOnSwap() {
  FrameDump& d = g_frame_dump;
  if (d.active) {
    d.active = false;
    FrameDumpFlushRun();
    std::string path = "frame_dump.txt";
#if defined(__linux__)
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = 0;
      std::string dir(exe);
      dir.resize(dir.rfind('/') + 1);
      path = dir + path;
    }
#endif
    if (std::FILE* file = std::fopen(path.c_str(), d.captures ? "a" : "w")) {
      const std::time_t t = std::time(nullptr);
      char stamp[16];
      std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&t));
      std::fprintf(file,
                   "===== frame dump %u at %s: %u draws (+%u second-tile draws merged), %u "
                   "transfers (+%u skipped), %u clears forwarded, %u resolves (%u direct to "
                   "texture), %u texture loads of GPU-written data, %u failed draws, %u "
                   "null-texture fetches =====\n",
                   d.captures + 1, stamp, d.frame_draws, d.frame_tile_draws_merged,
                   d.frame_transfers, d.frame_transfers_skipped, d.frame_clears_forwarded,
                   d.frame_resolves,
                   d.frame_direct_resolves,
                   d.frame_texture_loads, d.frame_draw_failures, d.frame_missing_textures);
      std::fwrite(d.text.data(), 1, d.text.size(), file);
      std::fputc('\n', file);
      std::fclose(file);
    }
    ++d.captures;
    d.text.clear();
  }
  if (!REXCVAR_GET(log_frame_dump) || d.captures >= 40) {
    return;
  }
  const double now =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (d.next_capture_time == 0.0) {
    d.next_capture_time = now + 20.0;
  } else if (now >= d.next_capture_time) {
    d.next_capture_time = now + 20.0;
    d.active = true;
    d.frame_draws = d.frame_transfers = d.frame_transfers_skipped = d.frame_resolves =
        d.frame_direct_resolves = d.frame_texture_loads = d.frame_clears_forwarded =
            d.frame_tile_draws_merged = d.frame_draw_failures = d.frame_missing_textures = 0;
    d.text.reserve(1 << 18);
  }
}

}  // namespace

void CommandProcessor::LogGuestFrameTiming() {
  FrameDumpOnSwap();
  using Clock = std::chrono::steady_clock;
  if (!REXCVAR_GET(log_guest_fps)) {
    // Off (the default): no per-frame bookkeeping at all; start a fresh window
    // if it is turned on later.
    fps_log_window_start_ = {};
    return;
  }
  constexpr auto kWindow = std::chrono::seconds(5);
  constexpr auto kSlowFrame = std::chrono::microseconds(33334);
  const Clock::time_point now = Clock::now();
  if (fps_log_window_start_ == Clock::time_point{}) {
    fps_log_window_start_ = now;
    fps_log_last_swap_ = now;
    fps_log_events_last_[0] = g_frame_events.texture_loads.load(std::memory_order_relaxed);
    fps_log_events_last_[1] = g_frame_events.upload_bytes.load(std::memory_order_relaxed);
    fps_log_events_last_[2] = g_frame_events.shader_translations.load(std::memory_order_relaxed);
    fps_log_events_last_[3] = g_frame_events.pipelines_created.load(std::memory_order_relaxed);
    return;
  }
  const Clock::duration frame = now - fps_log_last_swap_;
  fps_log_last_swap_ = now;
  ++fps_log_frames_;
  HostPerfSampleFrame();
  const uint64_t events_now[4] = {
      g_frame_events.texture_loads.load(std::memory_order_relaxed),
      g_frame_events.upload_bytes.load(std::memory_order_relaxed),
      g_frame_events.shader_translations.load(std::memory_order_relaxed),
      g_frame_events.pipelines_created.load(std::memory_order_relaxed)};
  if (frame > fps_log_worst_) {
    for (int i = 0; i < 4; ++i) {
      fps_log_worst_events_[i] = events_now[i] - fps_log_events_last_[i];
    }
  }
  std::memcpy(fps_log_events_last_, events_now, sizeof(events_now));
  fps_log_worst_ = std::max(fps_log_worst_, frame);
  if (frame > kSlowFrame) {
    ++fps_log_slow_frames_;
  }
  const Clock::duration window = now - fps_log_window_start_;
  if (window < kWindow) {
    return;
  }
  ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
  const uint64_t presents = presenter ? presenter->present_count() : 0;
  if (REXCVAR_GET(log_guest_fps)) {
    const double seconds = std::chrono::duration<double>(window).count();
    const std::string host = HostPerfReport(seconds);
    static constexpr const char* kSyncNames[] = {"occlusion query", "memexport readback",
                                                 "resolve readback", "pipeline wait",
                                                 "frame sync"};
    std::string syncs;
    for (uint32_t i = 0; i < uint32_t(GpuSyncKind::kCount); ++i) {
      if (gpu_sync_count_[i]) {
        syncs += fmt::format("{}{} {} ({:.1f} ms)", syncs.empty() ? "" : ", ", kSyncNames[i],
                             gpu_sync_count_[i],
                             std::chrono::duration<double, std::milli>(gpu_sync_time_[i]).count());
      }
    }
    // GPU execution time per guest frame (the GPU is the limit when this is
    // close to the frame time).
    const std::string gpu_time = TakeGpuTimeReport(fps_log_frames_);
    const std::string line = fmt::format(
        "{:.1f} fps over {:.1f} s, slowest frame {:.1f} ms ({} texture loads, {} KB uploaded, "
        "{} shaders translated, {} pipelines created), {} of {} frames over 33 ms; host "
        "presented {:.1f}/s{}; GPU waits: {}",
        fps_log_frames_ / seconds, seconds,
        std::chrono::duration<double, std::milli>(fps_log_worst_).count(),
        fps_log_worst_events_[0], fps_log_worst_events_[1] / 1024, fps_log_worst_events_[2],
        fps_log_worst_events_[3], fps_log_slow_frames_, fps_log_frames_,
        (presents - fps_log_presents_) / seconds, gpu_time, syncs.empty() ? "none" : syncs);
    REXGPU_INFO("[guest-fps] {}", line);
    if (!host.empty()) {
      REXGPU_INFO("[host-perf] {}", host);
    }
    HostPerfFileWrite(line, host);
  }
  fps_log_presents_ = presents;
  for (uint32_t i = 0; i < uint32_t(GpuSyncKind::kCount); ++i) {
    gpu_sync_count_[i] = 0;
    gpu_sync_time_[i] = {};
  }
  fps_log_window_start_ = now;
  fps_log_frames_ = 0;
  fps_log_slow_frames_ = 0;
  fps_log_worst_ = {};
  std::memset(fps_log_worst_events_, 0, sizeof(fps_log_worst_events_));
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  {
    REX_FRAME_ZONE(kLimiter);
    frame_limiter_.Pace(REXCVAR_GET(frame_limit));
  }
  LogGuestFrameTiming();

  // The small guest FPS readout also works in Release, where the full
  // profiling macros compile out. Shared runtime counters feed the F3 UI.
  auto mean_us = guest_frame_meter_.Record(GuestFrameMeter::Clock::now());
  rex::perf::SetCounter(rex::perf::CounterId::kFrameTimeUs, mean_us);
  rex::perf::SetCounter(rex::perf::CounterId::kFps, mean_us > 0 ? 1000000 / mean_us : 0);
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  rex::perf::ResetFrameCounters();
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  reader->AdvanceRead((count - 4) * sizeof(uint32_t));

  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  // One frame recorder row per guest swap, ending once the swap is issued.
  // The level follows the cvar here, so it can change while the game runs.
  rex_frame_recorder_end_frame();
  rex_frame_recorder_set_level(static_cast<uint32_t>(std::max(0, REXCVAR_GET(frame_recorder))));

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;

  bool matched = false;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (!matched) {
      // Wait.
      if (wait >= 0x100) {
        // Diagnostic: per-second summary of WAIT_REG_MEM wait-hint sleeps
        // (the vsync path that sleeps wait/0x100 ms). Long, frequent
        // sleeps here mean the guest's own frame loop is pacing itself.
        static uint64_t diag_waits = 0;
        static uint64_t diag_sleep_ms = 0;
        static auto diag_last_log =
            std::chrono::steady_clock::now();
        diag_waits++;
        if (REXCVAR_GET(vsync)) {
          diag_sleep_ms += wait / 0x100;
        }
        const auto now_log = std::chrono::steady_clock::now();
        if (now_log - diag_last_log >= std::chrono::seconds(1)) {
          const auto elapsed_ms = std::chrono::duration_cast<
              std::chrono::milliseconds>(now_log - diag_last_log)
                                      .count();
          REXGPU_INFO(
              "WAIT_REG_MEM long-waits: {} in {} ms "
              "(~{:.1f}/s, total sleep {} ms/s, vsync={})",
              diag_waits, elapsed_ms,
              1000.0 * static_cast<double>(diag_waits) /
                  static_cast<double>(elapsed_ms),
              diag_sleep_ms, REXCVAR_GET(vsync) ? 1 : 0);
          diag_waits = 0;
          diag_sleep_ms = 0;
          diag_last_log = now_log;
        }
        PrepareForWait();
        {
          REX_FRAME_ZONE(kRegWait);
          if (!REXCVAR_GET(vsync)) {
            // User wants it fast and dangerous.
            rex::thread::MaybeYield();
          } else {
            auto sleep = std::chrono::microseconds(std::chrono::milliseconds(wait / 0x100));
            const int32_t poll_us = REXCVAR_GET(wait_reg_mem_poll_us);
            if (poll_us > 0) {
              sleep = std::min(sleep, std::chrono::microseconds(poll_us));
            }
            rex::thread::Sleep(sleep);
          }
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    memory::store(memory_->TranslatePhysical(addr), write_data);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  memory::store(memory_->TranslatePhysical(address), data_value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::countof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // Set by D3D as BE but struct ABI is LE
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!pSampleCounts) {
      return true;
    }
    // 0xFFFFFEED is written to this two locations by D3D only on D3DISSUE_END
    // and used to detect a finished query.
    bool is_end_via_z_pass =
        pSampleCounts->ZPass_A == kQueryFinished || pSampleCounts->ZPass_B == kQueryFinished;
    // Older versions of D3D also checks for ZFail (4D5307D5).
    bool is_end_via_z_fail =
        pSampleCounts->ZFail_A == kQueryFinished || pSampleCounts->ZFail_B == kQueryFinished;
    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      if (!draw_succeeded) {
        auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
        REXGPU_ERROR(
            "{}({}, {}, {}): Failed in backend "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
            uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
            uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
            uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
            uint32_t(rb_modecontrol.edram_mode));
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  // PM4_DRAW_INDX is a regular indexed draw (may carry a viz query token). The
  // hero/dog EDRAM-resolve copy is NOT issued with this opcode, so never
  // force-readback from it.
  may_require_readback_resolve_ = false;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  // This is the opcode the EDRAM-resolve copy (e.g. the hero/dog texture blit)
  // is issued with, so it is the only one that may force a readback.
  may_require_readback_resolve_ = true;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_INFO("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_INFO("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    if (id < 32) {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
    }
  }

  return true;
}

}  // namespace rex::graphics
