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

#include "gpu/xenos/xenos_graphics_driver_base.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/string/text.h>
#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/vblank_pacing.h"
#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/graphics_system.h>
#include <rex/paths.h>
#include <rex/xenos/register.h>
#include <rex/driver_context.h>
#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/presenter.h"

namespace {

rex::gpu::CommandProcessor::SwapPostEffect ParseSwapPostEffect(const std::string& effect_name) {
  std::string lowered = rex::string::utf8_lower_ascii(effect_name);
  std::ranges::replace(lowered, '-', '_');
  if (lowered == "fxaa") {
    return rex::gpu::CommandProcessor::SwapPostEffect::kFXAA;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::gpu::CommandProcessor::SwapPostEffect::kFXAAExtreme;
  }
  return rex::gpu::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::gpu {

XenosGraphicsDriverBase::XenosGraphicsDriverBase() : vsync_worker_running_(false) {}

XenosGraphicsDriverBase::~XenosGraphicsDriverBase() {}

void XenosGraphicsDriverBase::SetGraphicsSettings(GraphicsSettings* settings) {
  provided_settings_ = settings;
}

Result<void> XenosGraphicsDriverBase::Initialize(IDriverContext& context) {
  if (settings_ != nullptr) {
    return Ok();
  }
  if (auto registered = context.GetSettings().Register(*this); !registered) {
    return Err(ErrorCategory::kConfig,
               std::format("gpu.xenos settings: {}", registered.error().message));
  }
  settings_ = &context.GetSettings();
  graphics_settings_ = provided_settings_;
  return Ok();
}

Result<void> XenosGraphicsDriverBase::SetSurface(const NativeWindow& window) {
  CreateProvider(true);
  if (!provider_) {
    return Err(ErrorCategory::kRuntime, "Unable to create graphics provider");
  }
  presentation_ = XenosPresentation::Create(
      window, *provider_, *this,
      [this](bool is_responsible, [[maybe_unused]] bool statically_from_ui_thread) {
        OnHostGPULossFromAnyThread(is_responsible);
      });
  if (!presentation_) {
    provider_.reset();
    return Err(ErrorCategory::kRuntime, "Unable to create presenter");
  }
  return Ok();
}

void XenosGraphicsDriverBase::ResizeSurface(u32 width, u32 height) {
  if (presentation_) {
    presentation_->Resize(width, height);
  }
}

bool XenosGraphicsDriverBase::BeginPresent() {
  return presentation_ && presentation_->ConnectToWindow();
}

void XenosGraphicsDriverBase::RenderOverlays(ImDrawData& draw_data) {
  if (presentation_) {
    presentation_->SetOverlays(draw_data);
  }
}

void XenosGraphicsDriverBase::EndPresent() {
  if (presentation_) {
    presentation_->Present();
  }
}

bool XenosGraphicsDriverBase::IsVSyncEnabled() const {
  return graphics_settings_ != nullptr ? graphics_settings_->vsync.Get() : true;
}

u32 XenosGraphicsDriverBase::GetResolutionScaleX() const {
  return static_cast<u32>(std::max<i32>(
      1, graphics_settings_ != nullptr ? graphics_settings_->resolution_scale_x.Get() : 1));
}

u32 XenosGraphicsDriverBase::GetResolutionScaleY() const {
  return static_cast<u32>(std::max<i32>(
      1, graphics_settings_ != nullptr ? graphics_settings_->resolution_scale_y.Get() : 1));
}

i32 XenosGraphicsDriverBase::GetAnisotropicOverride() const {
  return graphics_settings_ != nullptr ? graphics_settings_->anisotropic_override.Get() : 3;
}

Result<void> XenosGraphicsDriverBase::AttachTitle(const TitleInfo& info, IPhysicalMemory& memory,
                                                  IPhysicalWriteWatch& watch,
                                                  IGuestInterrupts& interrupts) {
  if (settings_ == nullptr) {
    return Err(ErrorCategory::kRuntime, "AttachTitle needs Initialize first");
  }
  title_info_ = info;
  physical_memory_ = &memory;
  write_watch_ = &watch;
  guest_interrupts_ = &interrupts;

  if (!provider_) {
    CreateProvider(false);
  }

  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXLOG_ERROR(gpu, "Unable to initialize command processor");
    return Err(ErrorCategory::kRuntime, "Unable to initialize command processor");
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(swap_post_effect.Get()));

  vsync_worker_running_ = true;
  auto thread = guest_interrupts_->StartHostThread("GPU VSync", [this](std::stop_token) {
    f64 refresh_rate_hz = std::max(1.0, static_cast<f64>(title_info_.video_mode.refresh_rate));
    u64 guest_tick_frequency = Clock::GetGuestTickFrequency();
    u64 vsync_interval_ticks =
        std::max(static_cast<u64>(1),
                 static_cast<u64>(static_cast<f64>(guest_tick_frequency) / refresh_rate_hz));
    u64 no_vsync_interval_ticks = std::max(static_cast<u64>(1), guest_tick_frequency / 1000);
    const f64 nanoseconds_per_tick = 1e9 / static_cast<f64>(guest_tick_frequency);
    constexpr std::chrono::milliseconds kLongestSleep{50};
    u64 last_frame_time = Clock::QueryGuestTickCount();
    while (vsync_worker_running_) {
      u64 interval_ticks = IsVSyncEnabled() ? vsync_interval_ticks : no_vsync_interval_ticks;
      DeliverVblanks(
          Clock::QueryGuestTickCount(), last_frame_time, interval_ticks, kMaxVblankCatchUp,
          [this] { MarkVblank(); }, [this] { return vsync_worker_running_.load(); });
      const u64 current_time = Clock::QueryGuestTickCount();
      const u64 next_time = last_frame_time + interval_ticks;
      const u64 remaining_ticks = next_time > current_time ? next_time - current_time : 0;
      rex::platform::SleepFor(std::min<std::chrono::nanoseconds>(
          std::chrono::nanoseconds(
              static_cast<i64>(static_cast<f64>(remaining_ticks) * nanoseconds_per_tick)),
          kLongestSleep));
    }
  });
  if (!thread) {
    vsync_worker_running_ = false;
    return Err(ErrorCategory::kRuntime,
               std::format("Unable to start the vsync thread, {}", thread.error().message));
  }
  vsync_worker_thread_ = std::move(*thread);

  if (const std::filesystem::path cache_root = REX_PATHS().cache_root.Get(); !cache_root.empty()) {
    REX_TRY_VOID(InitializeShaderStorage(cache_root, title_info_.title_id, false));
  }

  return Ok();
}

void XenosGraphicsDriverBase::DetachTitle() {
  vsync_worker_running_ = false;
  vsync_worker_thread_ = ScopeGuard();

  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }
  shader_storage_opened_ = false;
  physical_memory_ = nullptr;
  write_watch_ = nullptr;
  guest_interrupts_ = nullptr;
}

void XenosGraphicsDriverBase::Shutdown() {
  DetachTitle();

  presentation_.reset();
  provider_.reset();
  if (settings_ != nullptr) {
    graphics_settings_ = nullptr;
    settings_->Unregister(*this);
    settings_ = nullptr;
  }
}

void XenosGraphicsDriverBase::OnHostGPULossFromAnyThread([[maybe_unused]] bool is_responsible) {
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  REX_FATAL(gpu, "Graphics device lost (probably due to an internal error)");
}

u32 XenosGraphicsDriverBase::ReadRegister(u32 r) {
  if (physical_memory_ == nullptr) {
    return 0;
  }

  if (const auto fixed = xenos::ReadFixedRegister(r, title_info_.video_mode)) {
    return *fixed;
  }

  if (!register_file_.GetRegisterInfo(r)) {
    REXLOG_WARN_ONCE(gpu, unknown_read_once_, r, "GPU: Read from unknown register ({:04X})", r);
  }

  REX_ASSERT(r < xenos::RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void XenosGraphicsDriverBase::WriteRegister(u32 r, u32 value) {
  switch (r) {
    case static_cast<u32>(xenos::Register::CP_RB_WPTR):
      if (command_processor_) {
        command_processor_->UpdateWritePointer(value);
      }
      break;
    case static_cast<u32>(xenos::Register::D1GRPH_PRIMARY_SURFACE_ADDRESS):
      break;
    default:
      REXLOG_WARN_ONCE(gpu, unknown_write_once_, r, "Unknown GPU register {:04X} write: {:08X}", r,
                       value);
      break;
  }

  REX_ASSERT(r < xenos::RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void XenosGraphicsDriverBase::InitializeRingBuffer(u32 ptr, u32 size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void XenosGraphicsDriverBase::EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

FrameStatus XenosGraphicsDriverBase::WaitForFrame(u32 timeout_ms) {
  std::unique_lock lock(frame_mutex_);
  frame_signal_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return swap_count_ != swap_seen_; });
  FrameStatus status;
  status.new_frame = swap_count_ != swap_seen_;
  swap_seen_ = swap_count_;
  return status;
}

void XenosGraphicsDriverBase::NotifySwap() {
  {
    std::lock_guard lock(frame_mutex_);
    ++swap_count_;
  }
  frame_signal_.notify_all();
}

void XenosGraphicsDriverBase::DispatchInterruptCallback(u32 source, u32 cpu) {
  if (auto raised = guest_interrupts_->RaiseGraphicsInterrupt(source, cpu); !raised) {
    REXLOG_ERROR(gpu, "Graphics interrupt refused, {}", raised.error().message);
  }
}

void XenosGraphicsDriverBase::MarkVblank() {
  if (command_processor_) {
    command_processor_->IncrementCounter();
  }

  DispatchInterruptCallback(0, 2);
}

void XenosGraphicsDriverBase::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void XenosGraphicsDriverBase::InvalidateGPUMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGPUMemory(); });
}

Result<void> XenosGraphicsDriverBase::InitializeShaderStorage(const std::filesystem::path& root,
                                                              u32 title_id, bool blocking) {
  if (!store_shaders.Get() || shader_storage_opened_ || !command_processor_) {
    return Ok();
  }
  shader_storage_opened_ = true;
  if (blocking) {
    command_processor_->CallInThreadAndWait([this, root = std::filesystem::path(root), title_id]() {
      command_processor_->InitializeShaderStorage(root, title_id, true);
    });
  } else {
    command_processor_->CallInThread([this, root = std::filesystem::path(root), title_id]() {
      command_processor_->InitializeShaderStorage(root, title_id, false);
    });
  }
  return Ok();
}

}  // namespace rex::gpu
