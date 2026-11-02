/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "kernel/dispatcher.h"
#include "kernel/overlapped.h"
#include "kernel/thread_wait.h"
#include "kernel/guest_stack.h"

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/assert.h>
#include <rex/kernel/types/thread.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/ppc/seh.h>
#include <rex/platform/thread.h>
#include <rex/platform/timer.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/utility.h>
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

constexpr u32 kDeferredOverlappedDelayMillis = 100;
constexpr u32 kDPCStackSize = 64 * 1024;
constexpr u32 kDPCStackTopReserve = 0x100;
constexpr u32 kDPCListEntryOffset = 4;

thread_local u32 t_interrupt_depth = 0;
thread_local bool t_draining = false;

u32 ExtendedError(const CompletionOptions& options) {
  return options.extended_error.value_or(options.result);
}

u32 Length(const CompletionOptions& options) {
  return options.length.value_or(options.result ? 0xFFFFFFFF : 0);
}

}  // namespace

void EnterInterrupt() {
  ++t_interrupt_depth;
}

bool LeaveInterrupt() {
  return --t_interrupt_depth == 0;
}

Dispatcher::Dispatcher(TitleRuntime& runtime) : runtime_(runtime) {
  auto* memory = StateOf(runtime).GetMemory();
  dpc_list_head_ = memory->SystemHeapAlloc(sizeof(X_LIST_ENTRY));
  kernel::InitializeListHead(memory->TranslateVirtual<X_LIST_ENTRY*>(dpc_list_head_),
                             dpc_list_head_);
}

Dispatcher::~Dispatcher() {
  FreeGuestStack(*StateOf(runtime_).GetMemory(), dpc_stack_base_);
  if (dpc_list_head_) {
    StateOf(runtime_).GetMemory()->SystemHeapFree(dpc_list_head_);
  }
}

void Dispatcher::CompleteOverlappedEx(u32 overlapped, X_RESULT result, u32 extended_error,
                                      u32 length) {
  auto ptr = StateOf(runtime_).GetMemory()->TranslateVirtual(overlapped);
  rex::kernel::PublishOverlapped([&] {
    XOverlappedSetExtendedError(ptr, extended_error);
    XOverlappedSetLength(ptr, length);
    XOverlappedSetResult(ptr, result);
    X_HANDLE event_handle = XOverlappedGetEvent(ptr);
    if (event_handle) {
      auto ev = runtime_.GetObjectTable().Get<EventObject>(event_handle);
      REX_ASSERT(static_cast<bool>(ev));
      if (ev) {
        ev->Set();
      }
    }
  });
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = runtime_.GetThreadRegistry().FindByHandle(thread_handle);
    if (thread) {
      u32 routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueAPC(routine, result, length, overlapped);
    }
  }
}

void Dispatcher::Publish(rex::MoveOnlyFunction<void()> publish) {
  rex::kernel::PublishOverlapped(publish);
}

u32 Dispatcher::Observe(rex::MoveOnlyFunction<u32()> observe) {
  return rex::kernel::ObserveOverlapped(observe);
}

void Dispatcher::Complete(u32 overlapped, const CompletionOptions& options) {
  auto ptr = StateOf(runtime_).GetMemory()->TranslateVirtual(overlapped);
  XOverlappedSetContext(ptr, KernelThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped, options.result, ExtendedError(options), Length(options));
}

void Dispatcher::CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<void()> work,
                                  const CompletionOptions& options,
                                  rex::MoveOnlyFunction<void()> pre,
                                  rex::MoveOnlyFunction<void()> post) {
  CompleteDeferred(
      overlapped,
      [work = std::move(work), options]() mutable {
        work();
        return options;
      },
      std::move(pre), std::move(post));
}

void Dispatcher::CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<CompletionOptions()> work,
                                  rex::MoveOnlyFunction<void()> pre,
                                  rex::MoveOnlyFunction<void()> post) {
  REXLOG_DEBUG(system, "CompleteDeferred: queuing for overlapped {:08X}", overlapped);
  auto ptr = StateOf(runtime_).GetMemory()->TranslateVirtual(overlapped);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, KernelThread::GetCurrentThreadHandle());
  std::lock_guard lock(dispatch_mutex_);
  dispatch_queue_.push_back([this, work = std::move(work), overlapped, pre = std::move(pre),
                             post = std::move(post)]() mutable {
    REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running pre", overlapped);
    if (pre) {
      pre();
    }
    REXLOG_DEBUG(system, "Deferred overlapped {:08X}: sleeping {}ms", overlapped,
                 kDeferredOverlappedDelayMillis);
    rex::platform::SleepFor(std::chrono::milliseconds(kDeferredOverlappedDelayMillis));
    REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running completion", overlapped);
    const CompletionOptions options = work();
    REXLOG_DEBUG(system, "Deferred overlapped {:08X}: completing with result {:08X}", overlapped,
                 options.result);
    CompleteOverlappedEx(overlapped, options.result, ExtendedError(options), Length(options));
    if (post) {
      REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running post", overlapped);
      post();
    }
  });
  dispatch_cond_.notify_all();
}

Dispatcher::DPCImpersonationScope Dispatcher::BeginDPCImpersonation(X_KPCR& pcr) {
  DPCImpersonationScope scope;
  scope.previous_irql = pcr.current_irql;
  pcr.current_irql = kIRQLDispatch;
  pcr.prcb_data.dpc_active = 1;
  return scope;
}

void Dispatcher::EndDPCImpersonation(X_KPCR& pcr, const DPCImpersonationScope& scope) {
  pcr.prcb_data.dpc_active = 0;
  pcr.current_irql = scope.previous_irql;
}

bool Dispatcher::Queue(u32 dpc, u32 arg1, u32 arg2) {
  auto* memory = StateOf(runtime_).GetMemory();
  std::lock_guard lock(dpc_mutex_);
  auto* object = memory->TranslateVirtual<X_KDPC*>(dpc);
  if (object->inserted_number) {
    return false;
  }
  object->inserted_number = 1;
  object->arg1 = arg1;
  object->arg2 = arg2;
  kernel::InsertTailList(dpc_list_head_, dpc + kDPCListEntryOffset, memory);
  return true;
}

bool Dispatcher::Remove(u32 dpc) {
  auto* memory = StateOf(runtime_).GetMemory();
  std::lock_guard lock(dpc_mutex_);
  auto* object = memory->TranslateVirtual<X_KDPC*>(dpc);
  if (!object->inserted_number) {
    return false;
  }
  kernel::RemoveEntryList(&object->list_entry, memory);
  object->inserted_number = 0;
  return true;
}

bool Dispatcher::HasQueuedDPCs() {
  auto* memory = StateOf(runtime_).GetMemory();
  std::lock_guard lock(dpc_mutex_);
  return !kernel::IsListEmpty(memory->TranslateVirtual<X_LIST_ENTRY*>(dpc_list_head_), memory);
}

std::optional<Dispatcher::QueuedDPC> Dispatcher::PopDPC() {
  auto* memory = StateOf(runtime_).GetMemory();
  std::lock_guard lock(dpc_mutex_);
  auto* head = memory->TranslateVirtual<X_LIST_ENTRY*>(dpc_list_head_);
  if (kernel::IsListEmpty(head, memory)) {
    return std::nullopt;
  }
  const u32 address = kernel::RemoveHeadList(head, memory) - kDPCListEntryOffset;
  auto* object = memory->TranslateVirtual<X_KDPC*>(address);
  object->inserted_number = 0;
  return QueuedDPC{address, object->routine, object->context, object->arg1, object->arg2};
}

void Dispatcher::RunQueuedDPCs(ThreadState& state) {
  auto* ctx = state.GetContext();
  auto& pcr =
      *StateOf(runtime_).GetMemory()->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  auto* functions = StateOf(runtime_).GetFunctionDispatcher();
  const DPCImpersonationScope scope = BeginDPCImpersonation(pcr);
  while (const std::optional<QueuedDPC> dpc = PopDPC()) {
    const u64 saved_r1 = ctx->r1.u64;
    ctx->r1.u64 = dpc_stack_top_;
    u64 args[] = {dpc->address, dpc->context, dpc->arg1, dpc->arg2};
    functions->ExecuteTrap(&state, dpc->routine, args, rex::CountOf(args));
    ctx->r1.u64 = saved_r1;
  }
  EndDPCImpersonation(pcr, scope);
}

void Dispatcher::Drain() {
  ThreadState* state = ThreadState::Get();
  if (!state || !state->GetContext()->r13.u64 || !dpc_stack_top_ || t_interrupt_depth ||
      t_draining) {
    return;
  }
  while (HasQueuedDPCs()) {
    bool expected = false;
    if (!draining_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    t_draining = true;
    RunQueuedDPCs(*state);
    t_draining = false;
    draining_.store(false, std::memory_order_release);
  }
}

void Dispatcher::Post(rex::MoveOnlyFunction<void()> work) {
  {
    std::lock_guard lock(dispatch_mutex_);
    dispatch_queue_.push_back(std::move(work));
  }
  dispatch_cond_.notify_all();
}

void Dispatcher::PostDrain() {
  Post([this] { Drain(); });
}

bool Dispatcher::IsDispatchThread() const {
  return dispatch_thread_ && KernelThread::IsInThread() &&
         KernelThread::GetCallingThread()->GetHandle() == dispatch_thread_;
}

bool Dispatcher::IsRunning() const {
  return dispatch_thread_running_.load();
}

ThreadPin Dispatcher::FindThread() const {
  return dispatch_thread_ ? runtime_.GetThreadRegistry().FindByHandle(dispatch_thread_)
                          : ThreadPin{};
}

bool Dispatcher::TrackTimerExpiry(std::weak_ptr<rex::platform::TimerQueueWaitItem> expiry) {
  std::lock_guard lock(expiry_mutex_);
  if (expiries_stopped_) {
    return false;
  }
  std::erase_if(timer_expiries_, [](const auto& item) { return item.expired(); });
  timer_expiries_.push_back(std::move(expiry));
  return true;
}

void Dispatcher::Start() {
  if (dispatch_thread_running_) {
    return;
  }
  {
    std::lock_guard lock(expiry_mutex_);
    expiries_stopped_ = false;
  }
  if (!dpc_stack_base_) {
    if (Result<GuestStack> stack =
            AllocateGuestStack(*StateOf(runtime_).GetMemory(), kDPCStackSize, 0, false)) {
      dpc_stack_base_ = stack->alloc_base;
      dpc_stack_top_ = stack->base - kDPCStackTopReserve;
    } else {
      REXLOG_ERROR(system, "Unable to allocate the DPC stack");
    }
  }
  dispatch_thread_running_ = true;
  auto thread = std::make_unique<KernelThread>(runtime_, 128 * 1024, 0, [this]() {
    std::unique_lock lock(dispatch_mutex_);
    for (;;) {
      dispatch_cond_.wait(lock,
                          [this] { return !dispatch_queue_.empty() || !dispatch_thread_running_; });
      if (!dispatch_thread_running_) {
        break;
      }
      auto fn = std::move(dispatch_queue_.front());
      dispatch_queue_.pop_front();
      REXLOG_DEV(system, "Dispatch thread processing queued item ({} remaining)",
                 dispatch_queue_.size());
      lock.unlock();
      try {
        fn();
      } catch (const rex::ThreadStop&) {
        throw;
      } catch (const std::exception& e) {
        REX_FATAL(system, "Dispatch thread: deferred completion threw '{}'", e.what());
      } catch (...) {
        REX_FATAL(system, "Dispatch thread: deferred completion threw non-std exception");
      }
      REXLOG_DEV(system, "Dispatch thread completed item");
      lock.lock();
    }
    return 0;
  });
  thread->SetName("Kernel Dispatch");
  Result<X_HANDLE> launched = runtime_.GetThreadRegistry().Launch(std::move(thread));
  if (!launched) {
    dispatch_thread_running_ = false;
    dispatch_thread_ = 0;
    REX_FATAL(system, "Failed to create kernel dispatch thread (status {:#x})",
              ToXStatus(launched));
  }
  dispatch_thread_ = *launched;
}

void Dispatcher::RequestStop() {
  if (!dispatch_thread_running_) {
    return;
  }
  {
    std::lock_guard lock(dispatch_mutex_);
    dispatch_thread_running_ = false;
  }
  dispatch_cond_.notify_all();
  if (ThreadPin thread = runtime_.GetThreadRegistry().FindByHandle(dispatch_thread_)) {
    thread->RequestStop();
  }
}

void Dispatcher::Stop(std::chrono::milliseconds interval) {
  {
    std::lock_guard lock(expiry_mutex_);
    expiries_stopped_ = true;
    for (const auto& expiry : timer_expiries_) {
      if (auto item = expiry.lock()) {
        item->Disarm();
      }
    }
    timer_expiries_.clear();
  }
  if (dispatch_thread_) {
    RequestStop();
    if (ThreadPin thread = runtime_.GetThreadRegistry().FindByHandle(dispatch_thread_)) {
      std::vector<ThreadPin> threads;
      threads.push_back(std::move(thread));
      WaitForThreadsToStop(threads, interval, [interval](const ThreadPin&) {
        REXLOG_WARN(system, "Kernel Dispatch thread still running after {} ms", interval.count());
      });
    }
    CloseOwned(runtime_.GetObjectTable(), dispatch_thread_);
    dispatch_thread_ = 0;
    std::lock_guard lock(dispatch_mutex_);
    dispatch_queue_.clear();
  }
}

}  // namespace rex::kernel
