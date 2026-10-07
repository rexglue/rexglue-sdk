#include "runtime/null_gpu.h"

#include <algorithm>
#include <chrono>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xthread.h>

namespace rex::runtime {

namespace {

constexpr u32 kWritePointerRegister = 0x01C5;
constexpr u32 kEDRAMTimingRegister = 0x0F00;
constexpr u32 kBCControlRegister = 0x0F01;
constexpr u32 kVerticalCounterRegister = 0x194C;
constexpr u32 kInterruptStatusRegister = 0x1951;
constexpr u32 kViewportSizeRegister = 0x1961;
constexpr u32 kViewportLimit = 0x0FFF;
constexpr u8 kInterruptCPU = 2;

u32 RegisterIndex(u32 address) {
  return (address & (kMMIOBlockSize - 1)) / 4;
}

}  // namespace

NullGPU::~NullGPU() {
  Shutdown();
}

Result<void> NullGPU::SetupPresentation(gpu::IUIThread&) {
  return Ok();
}

Result<void> NullGPU::SetupGuestGPU(FunctionDispatcher& dispatcher, Runtime& runtime) {
  dispatcher_ = &dispatcher;
  runtime_ = &runtime;
  AttachMMIO(MMIOBlock::GPU, {this, ReadRegisterThunk, WriteRegisterThunk});
  mmio_attached_ = true;
  vsync_running_.store(true, std::memory_order_release);
  auto record = std::make_unique<ThreadRecord>(runtime, 128 * 1024, 0, [this]() {
    const f64 refresh_hz = std::max(1.0, static_cast<f64>(runtime_->video_mode().refresh_rate));
    const u64 interval = std::max<u64>(
        1, static_cast<u64>(static_cast<f64>(Clock::guest_tick_frequency()) / refresh_hz));
    u64 last = Clock::QueryGuestTickCount();
    while (vsync_running_.load(std::memory_order_acquire)) {
      const u64 now = Clock::QueryGuestTickCount();
      while (now - last >= interval) {
        RaiseVblank();
        last += interval;
      }
      platform::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  });
  record->set_name("GPU VSync");
  auto launched = runtime.thread_registry().Launch(std::move(record));
  if (!launched) {
    vsync_running_.store(false, std::memory_order_release);
    return Err(ErrorCategory::Runtime, "the null GPU vsync thread cannot start");
  }
  vsync_thread_ = *launched;
  REXLOG_INFO(system, "no GPU module is loaded, the title runs on the null GPU without PM4");
  return Ok();
}

Result<std::unique_ptr<gpu::IPresenter>> NullGPU::CreatePresenter() {
  return Err<std::unique_ptr<gpu::IPresenter>>(ErrorCategory::NotImplemented,
                                               "the null GPU has no presentation");
}

Result<std::unique_ptr<gpu::IOverlayDrawer>> NullGPU::CreateOverlayDrawer(gpu::IPresenter&) {
  return Err<std::unique_ptr<gpu::IOverlayDrawer>>(ErrorCategory::NotImplemented,
                                                   "the null GPU has no presentation");
}

void NullGPU::SetInterruptCallback(u32 callback, u32 user_data) {
  callback_data_.store(user_data, std::memory_order_relaxed);
  callback_.store(callback, std::memory_order_release);
}

void NullGPU::InitializeRingBuffer(u32, u32) {}

void NullGPU::EnableReadPointerWriteBack(u32 ptr, u32) {
  read_pointer_writeback_.store(ptr, std::memory_order_release);
}

Result<void> NullGPU::InitializeShaderStorage(const std::filesystem::path&, u32, bool) {
  return Ok();
}

void NullGPU::Shutdown() {
  if (mmio_attached_) {
    DetachMMIO(MMIOBlock::GPU, this);
    mmio_attached_ = false;
  }
  if (vsync_thread_) {
    vsync_running_.store(false, std::memory_order_release);
    WaitOn(*runtime_, vsync_thread_, 0, 0, 0, nullptr);
    (void)runtime_->object_table().Close(vsync_thread_);
    vsync_thread_ = 0;
  }
}

u32 NullGPU::ReadRegisterThunk(void* context, u32 address) {
  return static_cast<NullGPU*>(context)->ReadRegister(address);
}

void NullGPU::WriteRegisterThunk(void* context, u32 address, u32 value) {
  static_cast<NullGPU*>(context)->WriteRegister(address, value);
}

u32 NullGPU::ReadRegister(u32 address) const {
  const u32 index = RegisterIndex(address);
  const VideoMode& mode = runtime_->video_mode();
  switch (index) {
    case kEDRAMTimingRegister:
      return 0x08100748;
    case kBCControlRegister:
      return 0x0000200E;
    case kVerticalCounterRegister:
      return std::min(mode.height, kViewportLimit);
    case kInterruptStatusRegister:
      return 1;
    case kViewportSizeRegister:
      return (std::min(mode.width, kViewportLimit) << 16) | std::min(mode.height, kViewportLimit);
    default:
      return registers_[index].load(std::memory_order_relaxed);
  }
}

void NullGPU::WriteRegister(u32 address, u32 value) {
  const u32 index = RegisterIndex(address);
  registers_[index].store(value, std::memory_order_relaxed);
  if (index != kWritePointerRegister) {
    return;
  }
  const u32 writeback = read_pointer_writeback_.load(std::memory_order_acquire);
  if (writeback != 0) {
    *runtime_->memory()->TranslatePhysical<be_u32*>(writeback) = value;
  }
}

void NullGPU::RaiseVblank() {
  const u32 callback = callback_.load(std::memory_order_acquire);
  if (callback == 0) {
    return;
  }
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();
  thread->SetActiveCPU(kInterruptCPU);
  u64 args[] = {0, callback_data_.load(std::memory_order_relaxed)};
  dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, std::size(args));
}

}  // namespace rex::runtime
