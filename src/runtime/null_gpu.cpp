#include "runtime/null_gpu.h"

#include <algorithm>
#include <chrono>

#include <rex/chrono/clock.h>
#include <rex/logging/log.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xthread.h>
#include <rex/gpu/fixed_registers.h>
#include <rex/xenos/registers.h>

namespace rex::runtime {

namespace {

constexpr u32 kWritePointerRegister = static_cast<u32>(xenos::Register::CP_RB_WPTR);
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
  AttachMMIO(MMIOBlock::kGPU, {this, ReadRegisterThunk, WriteRegisterThunk});
  mmio_attached_ = true;
  vsync_running_.store(true, std::memory_order_release);
  auto thread = std::make_unique<KernelThread>(runtime, 128 * 1024, 0, [this]() {
    const f64 refresh_hz = std::max(1.0, static_cast<f64>(runtime_->GetVideoMode().refresh_rate));
    const u64 interval = std::max<u64>(
        1, static_cast<u64>(static_cast<f64>(Clock::GetGuestTickFrequency()) / refresh_hz));
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
  thread->SetName("GPU VSync");
  auto launched = runtime.GetThreadRegistry().Launch(std::move(thread));
  if (!launched) {
    vsync_running_.store(false, std::memory_order_release);
    return Err(ErrorCategory::kRuntime, "the null GPU vsync thread cannot start");
  }
  vsync_thread_ = *launched;
  REXLOG_INFO(system, "no GPU module is loaded, the title runs on the null GPU without PM4");
  return Ok();
}

Result<std::unique_ptr<gpu::IPresenter>> NullGPU::CreatePresenter() {
  return Err<std::unique_ptr<gpu::IPresenter>>(ErrorCategory::kNotImplemented,
                                               "the null GPU has no presentation");
}

Result<std::unique_ptr<gpu::IOverlayDrawer>> NullGPU::CreateOverlayDrawer(gpu::IPresenter&) {
  return Err<std::unique_ptr<gpu::IOverlayDrawer>>(ErrorCategory::kNotImplemented,
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
    DetachMMIO(MMIOBlock::kGPU, this);
    mmio_attached_ = false;
  }
  if (vsync_thread_) {
    vsync_running_.store(false, std::memory_order_release);
    Wait(*runtime_, vsync_thread_, 0, 0, 0, nullptr);
    CloseOwned(runtime_->GetObjectTable(), vsync_thread_);
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
  if (const auto fixed = gpu::ReadFixedRegister(index, runtime_->GetVideoMode())) {
    return *fixed;
  }
  return registers_[index].load(std::memory_order_relaxed);
}

void NullGPU::WriteRegister(u32 address, u32 value) {
  const u32 index = RegisterIndex(address);
  registers_[index].store(value, std::memory_order_relaxed);
  if (index != kWritePointerRegister) {
    return;
  }
  const u32 writeback = read_pointer_writeback_.load(std::memory_order_acquire);
  if (writeback != 0) {
    *runtime_->GetMemory()->TranslatePhysical<be_u32*>(writeback) = value;
  }
}

void NullGPU::RaiseVblank() {
  const u32 callback = callback_.load(std::memory_order_acquire);
  if (callback == 0) {
    return;
  }
  KernelThread* thread = KernelThread::GetCurrentThread();
  thread->SetActiveCPU(kInterruptCPU);
  u64 args[] = {0, callback_data_.load(std::memory_order_relaxed)};
  dispatcher_->ExecuteInterrupt(thread->GetThreadState(), callback, args, std::size(args));
}

}  // namespace rex::runtime
