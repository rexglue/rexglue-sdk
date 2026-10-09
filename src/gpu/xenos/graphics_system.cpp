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

#include <rex/cvar.h>
#include "gpu/xenos/command_processor.h"
#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/mmio.h>
#include <rex/runtime.h>
#include <rex/runtime/xthread.h>
#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/presenter.h"

#include "gpu/xenos/cvar_table.h"

namespace {

rex::gpu::CommandProcessor::SwapPostEffect ParseSwapPostEffect(const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
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
  rex::DetachMMIO(rex::MMIOBlock::GPU, this);
}

Result<void> GraphicsSystem::SetupPresentation(gpu::IUIThread& ui_thread) {
  if (presenter_) {
    return Ok();
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXLOG_ERROR(gpu, "Unable to create graphics provider");
      return Err(ErrorCategory::Runtime, "Unable to create graphics provider");
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGPU built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXLOG_ERROR(gpu,
                 "SetupPresentation called after headless SetupGuestGPU; call order is reversed");
    return Err(ErrorCategory::Runtime,
               "SetupPresentation called after headless SetupGuestGPU; call order is reversed");
  }

  ui_thread_ = &ui_thread;
  auto loss_cb = [this](bool is_responsible, [[maybe_unused]] bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  ui_thread.RunSynchronous([this, loss_cb] { presenter_ = provider_->CreatePresenter(loss_cb); });

  if (!presenter_) {
    REXLOG_ERROR(gpu, "Unable to create presenter");
    return Err(ErrorCategory::Runtime, "Unable to create presenter");
  }
  return Ok();
}

Result<void> GraphicsSystem::SetupGuestGPU(runtime::FunctionDispatcher& dispatcher,
                                           Runtime& runtime) {
  memory_ = dispatcher.memory();
  function_dispatcher_ = &dispatcher;
  runtime_ = &runtime;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider only for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXLOG_ERROR(gpu, "Unable to initialize command processor");
    return Err(ErrorCategory::Runtime, "Unable to initialize command processor");
  }
  command_processor_->SetDesiredSwapPostEffect(
      ParseSwapPostEffect(REXCVAR_GET(::rex::gpu::cvars::swap_post_effect)));

  rex::AttachMMIO(rex::MMIOBlock::GPU, {this, ReadRegisterThunk, WriteRegisterThunk});

  // Guest vblank timer based on the configured guest video mode.
  vsync_worker_running_ = true;
  auto record = std::make_unique<runtime::ThreadRecord>(*runtime_, 128 * 1024, 0, [this]() {
    double refresh_rate_hz =
        std::max(1.0, static_cast<double>(runtime_->video_mode().refresh_rate));
    uint64_t guest_tick_frequency = Clock::guest_tick_frequency();
    uint64_t vsync_interval_ticks = std::max(
        static_cast<uint64_t>(1),
        static_cast<uint64_t>(static_cast<double>(guest_tick_frequency) / refresh_rate_hz));
    uint64_t no_vsync_interval_ticks =
        std::max(static_cast<uint64_t>(1), guest_tick_frequency / 1000);
    uint64_t last_frame_time = Clock::QueryGuestTickCount();
    while (vsync_worker_running_) {
      uint64_t current_time = Clock::QueryGuestTickCount();
      uint64_t interval_ticks =
          REXCVAR_GET(::rex::gpu::cvars::vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
      while (current_time - last_frame_time >= interval_ticks) {
        MarkVblank();
        last_frame_time += interval_ticks;
      }
      rex::platform::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  });
  record->set_name("GPU VSync");
  vsync_worker_thread_ = runtime_->thread_registry().Launch(std::move(record)).value_or(0);

  return Ok();
}

Result<std::unique_ptr<gpu::IPresenter>> GraphicsSystem::CreatePresenter() {
  if (!provider_) {
    return Err<std::unique_ptr<gpu::IPresenter>>(ErrorCategory::Runtime,
                                                 "CreatePresenter needs SetupPresentation first");
  }
  std::unique_ptr<gpu::Presenter> presenter = provider_->CreatePresenter();
  if (!presenter) {
    return Err<std::unique_ptr<gpu::IPresenter>>(
        ErrorCategory::Runtime, "the graphics provider could not create a presenter");
  }
  return std::unique_ptr<gpu::IPresenter>(std::move(presenter));
}

Result<std::unique_ptr<gpu::IOverlayDrawer>> GraphicsSystem::CreateOverlayDrawer(
    gpu::IPresenter& presenter) {
  if (!provider_) {
    return Err<std::unique_ptr<gpu::IOverlayDrawer>>(
        ErrorCategory::Runtime, "CreateOverlayDrawer needs SetupPresentation first");
  }
  std::unique_ptr<gpu::ImmediateDrawer> drawer = provider_->CreateImmediateDrawer();
  if (!drawer) {
    return Err<std::unique_ptr<gpu::IOverlayDrawer>>(
        ErrorCategory::Runtime, "the graphics provider could not create a drawer");
  }
  drawer->SetPresenter(static_cast<gpu::Presenter*>(&presenter));
  return std::unique_ptr<gpu::IOverlayDrawer>(std::move(drawer));
}

void GraphicsSystem::Shutdown() {
  rex::DetachMMIO(rex::MMIOBlock::GPU, this);
  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    runtime::WaitOn(*runtime_, vsync_worker_thread_, 0, 0, 0, nullptr);
    (void)runtime_->object_table().Close(vsync_worker_thread_);
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
    // If there's no UI thread (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
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

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      return std::min(runtime_->video_mode().height, static_cast<uint32_t>(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      const runtime::VideoMode& video_mode = runtime_->video_mode();
      uint32_t viewport_width = std::min(video_mode.width, static_cast<uint32_t>(0x0FFF));
      uint32_t viewport_height = std::min(video_mode.height, static_cast<uint32_t>(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXLOG_DEBUG(gpu, "GPU: Read from unknown register ({:04X})", r);
      }
  }

  REX_ASSERT(r < xenos::RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
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

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  const u64 interrupt = interrupt_callback_.load(std::memory_order_acquire);
  const u32 callback = static_cast<u32>(interrupt);
  if (!callback) {
    return;
  }

  auto thread = runtime::ThreadRecord::GetCurrentThread();
  REX_ASSERT(thread != nullptr);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCPU(cpu);

  uint64_t args[] = {source, interrupt >> 32};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args,
                                         rex::countof(args));
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // REX_PROFILE_SCOPE("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

Result<void> GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& root,
                                                     u32 title_id, bool blocking) {
  if (!REXCVAR_GET(::rex::gpu::cvars::store_shaders)) {
    return Ok();
  }
  if (blocking) {
    command_processor_->CallInThreadAndWait([this, root, title_id]() {
      command_processor_->InitializeShaderStorage(root, title_id, true);
    });
  } else {
    command_processor_->CallInThread([this, root, title_id]() {
      command_processor_->InitializeShaderStorage(root, title_id, false);
    });
  }
  return Ok();
}

}  // namespace rex::gpu
