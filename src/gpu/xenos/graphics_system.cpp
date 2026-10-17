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

#include "gpu/xenos/graphics_system.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/string/utf8.h>
#include <rex/cvar.h>
#include "gpu/xenos/command_processor.h"
#include <rex/assert.h>
#include <rex/logging/flush.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/mmio.h>
#include <rex/runtime.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xthread.h>
#include <rex/gpu/fixed_registers.h>
#include <rex/xenos/registers.h>
#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/presenter.h"

#include "gpu/xenos/cvar_table_gpu.h"
#include "gpu/xenos/cvar_table_xenos.h"

namespace {

rex::gpu::CommandProcessor::SwapPostEffect ParseSwapPostEffect(const std::string& effect_name) {
  std::string lowered = rex::utf8_lower_ascii(effect_name);
  std::ranges::replace(lowered, '-', '_');
  if (lowered == "fxaa") {
    return rex::gpu::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::gpu::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::gpu {

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() {
  rex::DetachMMIO(rex::MMIOBlock::kGPU, this);
}

Result<void> GraphicsSystem::SetupPresentation(gpu::IUIThread& ui_thread) {
  if (presenter_) {
    return Ok();
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXLOG_ERROR(gpu, "Unable to create graphics provider");
      return Err(ErrorCategory::kRuntime, "Unable to create graphics provider");
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // Backends like Vulkan need swapchain support set at provider creation.
    REXLOG_ERROR(gpu,
                 "SetupPresentation called after headless SetupGuestGPU; call order is reversed");
    return Err(ErrorCategory::kRuntime,
               "SetupPresentation called after headless SetupGuestGPU; call order is reversed");
  }

  ui_thread_ = &ui_thread;
  auto loss_cb = [this](bool is_responsible, [[maybe_unused]] bool statically_from_ui_thread) {
    OnHostGPULossFromAnyThread(is_responsible);
  };
  ui_thread.RunSynchronous([this, loss_cb] { presenter_ = provider_->CreatePresenter(loss_cb); });

  if (!presenter_) {
    REXLOG_ERROR(gpu, "Unable to create presenter");
    return Err(ErrorCategory::kRuntime, "Unable to create presenter");
  }
  return Ok();
}

Result<void> GraphicsSystem::SetupGuestGPU(runtime::FunctionDispatcher& dispatcher,
                                           Runtime& runtime) {
  memory_ = dispatcher.GetMemory();
  function_dispatcher_ = &dispatcher;
  runtime_ = &runtime;

  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXLOG_ERROR(gpu, "Unable to initialize command processor");
    return Err(ErrorCategory::kRuntime, "Unable to initialize command processor");
  }
  command_processor_->SetDesiredSwapPostEffect(
      ParseSwapPostEffect(REXCVAR_GET(::rex::gpu::cvars::swap_post_effect)));

  rex::AttachMMIO(rex::MMIOBlock::kGPU, {this, ReadRegisterThunk, WriteRegisterThunk});

  vsync_worker_running_ = true;
  auto thread = std::make_unique<runtime::KernelThread>(*runtime_, 128 * 1024, 0, [this]() {
    f64 refresh_rate_hz = std::max(1.0, static_cast<f64>(runtime_->GetVideoMode().refresh_rate));
    u64 guest_tick_frequency = Clock::GetGuestTickFrequency();
    u64 vsync_interval_ticks =
        std::max(static_cast<u64>(1),
                 static_cast<u64>(static_cast<f64>(guest_tick_frequency) / refresh_rate_hz));
    u64 no_vsync_interval_ticks = std::max(static_cast<u64>(1), guest_tick_frequency / 1000);
    u64 last_frame_time = Clock::QueryGuestTickCount();
    while (vsync_worker_running_) {
      u64 current_time = Clock::QueryGuestTickCount();
      u64 interval_ticks =
          REXCVAR_GET(::rex::gpu::cvars::vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
      while (current_time - last_frame_time >= interval_ticks) {
        MarkVblank();
        last_frame_time += interval_ticks;
      }
      rex::platform::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  });
  thread->SetName("GPU VSync");
  vsync_worker_thread_ = runtime_->GetThreadRegistry().Launch(std::move(thread)).value_or(0);

  return Ok();
}

Result<std::unique_ptr<gpu::IPresenter>> GraphicsSystem::CreatePresenter() {
  if (!provider_) {
    return Err<std::unique_ptr<gpu::IPresenter>>(ErrorCategory::kRuntime,
                                                 "CreatePresenter needs SetupPresentation first");
  }
  std::unique_ptr<gpu::Presenter> presenter = provider_->CreatePresenter();
  if (!presenter) {
    return Err<std::unique_ptr<gpu::IPresenter>>(
        ErrorCategory::kRuntime, "the graphics provider could not create a presenter");
  }
  return std::unique_ptr<gpu::IPresenter>(std::move(presenter));
}

Result<std::unique_ptr<gpu::IOverlayDrawer>> GraphicsSystem::CreateOverlayDrawer(
    gpu::IPresenter& presenter) {
  if (!provider_) {
    return Err<std::unique_ptr<gpu::IOverlayDrawer>>(
        ErrorCategory::kRuntime, "CreateOverlayDrawer needs SetupPresentation first");
  }
  std::unique_ptr<gpu::ImmediateDrawer> drawer = provider_->CreateImmediateDrawer();
  if (!drawer) {
    return Err<std::unique_ptr<gpu::IOverlayDrawer>>(
        ErrorCategory::kRuntime, "the graphics provider could not create a drawer");
  }
  drawer->SetPresenter(static_cast<gpu::Presenter*>(&presenter));
  return std::unique_ptr<gpu::IOverlayDrawer>(std::move(drawer));
}

void GraphicsSystem::Shutdown() {
  rex::DetachMMIO(rex::MMIOBlock::kGPU, this);
  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    runtime::Wait(*runtime_, vsync_worker_thread_, 0, 0, 0, nullptr);
    runtime::CloseOwned(runtime_->GetObjectTable(), vsync_worker_thread_);
    vsync_worker_thread_ = 0;
  }

  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (presenter_) {
    if (ui_thread_) {
      ui_thread_->RunSynchronous([this] { presenter_.reset(); });
    }
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGPULossFromAnyThread([[maybe_unused]] bool is_responsible) {
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  REX_FATAL(gpu, "Graphics device lost (probably due to an internal error)");
}

u32 GraphicsSystem::ReadRegisterThunk(void* context, u32 addr) {
  return static_cast<GraphicsSystem*>(context)->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* context, u32 addr, u32 value) {
  static_cast<GraphicsSystem*>(context)->WriteRegister(addr, value);
}

u32 GraphicsSystem::ReadRegister(u32 addr) {
  u32 r = (addr & 0xFFFF) / 4;

  if (const auto fixed = gpu::ReadFixedRegister(r, runtime_->GetVideoMode())) {
    return *fixed;
  }

  if (!register_file_.GetRegisterInfo(r)) {
    REXLOG_DEBUG(gpu, "GPU: Read from unknown register ({:04X})", r);
  }

  REX_ASSERT(r < xenos::RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(u32 addr, u32 value) {
  u32 r = (addr & 0xFFFF) / 4;

  switch (r) {
    case static_cast<u32>(xenos::Register::CP_RB_WPTR):
      command_processor_->UpdateWritePointer(value);
      break;
    case static_cast<u32>(xenos::Register::D1GRPH_PRIMARY_SURFACE_ADDRESS):
      break;
    default:
      REXLOG_WARN(gpu, "Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  REX_ASSERT(r < xenos::RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(u32 ptr, u32 size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(u32 callback, u32 user_data) {
  interrupt_callback_.store(callback | (u64{user_data} << 32), std::memory_order_release);
  REXLOG_DEBUG(gpu, "SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(u32 source, u32 cpu) {
  const u64 interrupt = interrupt_callback_.load(std::memory_order_acquire);
  const u32 callback = static_cast<u32>(interrupt);
  if (!callback) {
    return;
  }

  auto thread = runtime::KernelThread::GetCurrentThread();
  REX_ASSERT(thread != nullptr);

  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCPU(cpu);

  u64 args[] = {source, interrupt >> 32};
  function_dispatcher_->ExecuteInterrupt(thread->GetThreadState(), callback, args,
                                         rex::CountOf(args));
}

void GraphicsSystem::MarkVblank() {
  if (command_processor_) {
    command_processor_->IncrementCounter();
  }

  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGPUMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGPUMemory(); });
}

Result<void> GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& root,
                                                     u32 title_id, bool blocking) {
  if (!REXCVAR_GET(::rex::gpu::cvars::store_shaders)) {
    return Ok();
  }
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
