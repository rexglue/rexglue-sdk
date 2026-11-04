#include "gpu/graphics_system.h"

#include <algorithm>

#include <rex/logging/log.h>
#include <rex/platform.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/xenos/register.h>
#include <rex/utility.h>

#include "gpu/graphics_state.h"

namespace rex::gpu {

namespace {

constexpr auto kStatsWindow = std::chrono::seconds(1);

}  // namespace

rex::gpu::GraphicsSettings::GraphicsSettings() : SettingOwner("gpu") {}

GraphicsSystem::GraphicsSystem() : state_(std::make_unique<State>(*this)) {}

GraphicsSystem::~GraphicsSystem() {
  Stop();
}

bool GraphicsSystem::HasDriver() const {
  return state_->driver != nullptr;
}

std::string GraphicsSystem::GetDriverName() const {
  std::string name = settings_.driver.Get();
  if (name == "any") {
#if REX_PLATFORM_WIN32
    return "d3d12";
#else
    return "vulkan";
#endif
  }
  return name;
}

bool GraphicsSystem::HasXenos() const {
  return state_->xenos != nullptr;
}

void GraphicsSystem::ReportFrame() {
  state_->CountFrame();
}

rex::gpu::FrameStats GraphicsSystem::GetFrameStats() const {
  std::lock_guard lock(state_->stats_mutex);
  rex::gpu::FrameStats stats;
  stats.frame_count = state_->frame_count;
  const auto now = std::chrono::steady_clock::now();
  size_t recent = 0;
  std::chrono::steady_clock::time_point first = now;
  std::chrono::steady_clock::time_point last = now;
  for (const auto& time : state_->frame_times) {
    if (now - time > kStatsWindow) {
      continue;
    }
    if (recent == 0) {
      first = time;
    }
    last = time;
    ++recent;
  }
  stats.fps = static_cast<f64>(recent);
  if (recent > 1) {
    stats.frame_time_ms =
        std::chrono::duration<f64, std::milli>(last - first).count() / static_cast<f64>(recent - 1);
  }
  return stats;
}

void GraphicsSystem::State::CountFrame() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(stats_mutex);
  ++frame_count;
  frame_times.push_back(now);
  while (!frame_times.empty() && now - frame_times.front() > kStatsWindow) {
    frame_times.pop_front();
  }
}

void GraphicsSystem::State::WarnNoXenos(const char* what) {
  if (!warned.exchange(true)) {
    REXGPU_WARN("{} has no Xenos graphics driver to run it", what);
    ++warning_count;
  }
}

u32 GraphicsSystem::State::ReadNullRegister(u32 index) {
  if (memory == nullptr || index >= null_registers.size()) {
    return 0;
  }
  if (const auto fixed = xenos::ReadFixedRegister(index, title_info.video_mode)) {
    return *fixed;
  }
  return null_registers[index].load(std::memory_order_relaxed);
}

void GraphicsSystem::State::WriteNullRegister(u32 index, u32 value) {
  if (index >= null_registers.size()) {
    return;
  }
  null_registers[index].store(value, std::memory_order_relaxed);
  if (index != static_cast<u32>(xenos::Register::CP_RB_WPTR) || memory == nullptr) {
    return;
  }
  const u32 writeback = null_writeback.load(std::memory_order_acquire);
  if (writeback != 0) {
    (void)memory->StoreRelease32(writeback & 0x1FFFFFFF, value);
  }
}

void GraphicsSystem::SetUIPump(MoveOnlyFunction<bool(std::chrono::milliseconds)> serve) {
  state_->serve_ui = std::move(serve);
}

Result<void> GraphicsSystem::Start(rex::gpu::IGraphicsDriver* graphics_driver) {
  Stop();
  State& state = *state_;
  state.driver = graphics_driver;
  if (graphics_driver == nullptr) {
    state.xenos = nullptr;
    return {};
  }
  state.xenos = graphics_driver->AsXenos();
  state.present_finished = false;
  state.present_thread = std::jthread([&state](std::stop_token stop) {
    state.PresentLoop(std::move(stop));
    state.present_finished = true;
  });
  return {};
}

void GraphicsSystem::Stop() {
  State& state = *state_;
  if (state.present_thread.joinable()) {
    state.present_thread.request_stop();
    while (!state.present_finished.load() && state.serve_ui &&
           state.serve_ui(std::chrono::milliseconds{2})) {}
    state.present_thread.join();
  }
  state.driver = nullptr;
  state.xenos = nullptr;
}

Result<void> GraphicsSystem::AttachTitle(const TitleInfo& info, IPhysicalMemory& memory,
                                         IPhysicalWriteWatch& watch, IGuestInterrupts& interrupts) {
  if (state_->driver == nullptr) {
    state_->title_info = info;
    state_->memory = &memory;
    return {};
  }
  return state_->driver->AttachTitle(info, memory, watch, interrupts);
}

void GraphicsSystem::DetachTitle() {
  if (state_->driver != nullptr) {
    state_->driver->DetachTitle();
  } else {
    state_->memory = nullptr;
  }
}

void GraphicsSystem::SetOverlayHook(MoveOnlyFunction<void(IOverlayRenderer&)> hook) {
  std::lock_guard lock(state_->hook_mutex);
  state_->overlay_hook = std::move(hook);
}

void GraphicsSystem::OnSurfaceResized(u32 width, u32 height) {
  std::lock_guard lock(state_->resize_mutex);
  state_->width = width;
  state_->height = height;
  state_->resize_pending = true;
}

void GraphicsSystem::InitializeRingBuffer(u32 address, u32 size_log2) {
  if (state_->xenos == nullptr) {
    if (state_->driver != nullptr) {
      state_->WarnNoXenos("VdInitializeRingBuffer");
    }
    return;
  }
  state_->xenos->InitializeRingBuffer(address, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(u32 address, u32 block_size_log2) {
  if (state_->xenos == nullptr) {
    if (state_->driver != nullptr) {
      state_->WarnNoXenos("VdEnableRingBufferRPtrWriteBack");
    } else {
      state_->null_writeback.store(address, std::memory_order_release);
    }
    return;
  }
  state_->xenos->EnableReadPointerWriteBack(address, block_size_log2);
}

u32 GraphicsSystem::ReadRegister(u32 index) {
  if (state_->xenos != nullptr) {
    return state_->xenos->ReadRegister(index);
  }
  return state_->driver == nullptr ? state_->ReadNullRegister(index) : 0;
}

void GraphicsSystem::WriteRegister(u32 index, u32 value) {
  if (state_->xenos != nullptr) {
    state_->xenos->WriteRegister(index, value);
  } else if (state_->driver == nullptr) {
    state_->WriteNullRegister(index, value);
  }
}

}  // namespace rex::gpu
