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

#include "runtime/dispatcher.h"
#include "runtime/guest_stack.h"

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/util/native_list.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xthread.h>

namespace rex::runtime {

namespace {

constexpr u32 kDeferredOverlappedDelayMillis = 100;
constexpr u32 kDPCStackSize = 64 * 1024;
constexpr u32 kDPCStackTopReserve = 0x100;
constexpr u32 kDPCListEntryOffset = 4;

thread_local u32 t_interrupt_depth = 0;
thread_local bool t_draining = false;

}  // namespace

void EnterInterrupt() {
  ++t_interrupt_depth;
}

bool LeaveInterrupt() {
  return --t_interrupt_depth == 0;
}

Dispatcher::Dispatcher(Runtime& runtime) : runtime_(runtime) {
  auto* memory = runtime.memory();
  dpc_list_head_ = memory->SystemHeapAlloc(sizeof(X_LIST_ENTRY));
  util::XeInitializeListHead(memory->TranslateVirtual<X_LIST_ENTRY*>(dpc_list_head_),
                             dpc_list_head_);
}

Dispatcher::~Dispatcher() {
  FreeGuestStack(*runtime_.memory(), dpc_stack_base_);
  if (dpc_list_head_) {
    runtime_.memory()->SystemHeapFree(dpc_list_head_);
  }
}

void Dispatcher::CompleteOverlappedEx(u32 overlapped_ptr, X_RESULT result, u32 extended_error,
                                      u32 length) {
  auto ptr = runtime_.memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = runtime_.object_table().Get<EventObject>(event_handle);
    assert_true(static_cast<bool>(ev));
    if (ev) {
      EventSet(*ev);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = runtime_.thread_registry().FindByHandle(thread_handle);
    if (thread) {
      u32 routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueAPC(routine, result, length, overlapped_ptr);
    }
  }
}

void Dispatcher::CompleteOverlappedImmediate(u32 overlapped_ptr, X_RESULT result) {
  u32 length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void Dispatcher::CompleteOverlappedImmediateEx(u32 overlapped_ptr, X_RESULT result,
                                               u32 extended_error, u32 length) {
  auto ptr = runtime_.memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, ThreadRecord::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void Dispatcher::CompleteOverlappedDeferred(std::function<void()> completion_callback,
                                            u32 overlapped_ptr, X_RESULT result,
                                            std::function<void()> pre_callback,
                                            std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr, result, result, 0,
                               pre_callback, post_callback);
}

void Dispatcher::CompleteOverlappedDeferredEx(std::function<void()> completion_callback,
                                              u32 overlapped_ptr, X_RESULT result,
                                              u32 extended_error, u32 length,
                                              std::function<void()> pre_callback,
                                              std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](u32& cb_extended_error,
                                                            u32& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void Dispatcher::CompleteOverlappedDeferred(std::function<X_RESULT()> completion_callback,
                                            u32 overlapped_ptr, std::function<void()> pre_callback,
                                            std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](u32& extended_error, u32& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<u32>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void Dispatcher::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(u32&, u32&)> completion_callback, u32 overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  REXLOG_DEBUG(system, "CompleteOverlappedDeferredEx: queuing for overlapped {:08X}",
               overlapped_ptr);
  auto ptr = runtime_.memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, ThreadRecord::GetCurrentThreadHandle());
  std::lock_guard lock(dispatch_mutex_);
  dispatch_queue_.push_back(
      [this, completion_callback, overlapped_ptr, pre_callback, post_callback]() {
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running pre_callback", overlapped_ptr);
        if (pre_callback) {
          pre_callback();
        }
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: sleeping {}ms", overlapped_ptr,
                     kDeferredOverlappedDelayMillis);
        rex::platform::Sleep(std::chrono::milliseconds(kDeferredOverlappedDelayMillis));
        u32 extended_error, length;
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running completion", overlapped_ptr);
        auto result = completion_callback(extended_error, length);
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: completing with result {:08X}",
                     overlapped_ptr, result);
        CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
        if (post_callback) {
          REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running post_callback", overlapped_ptr);
          post_callback();
        }
      });
  dispatch_cond_.notify_all();
}

Dispatcher::DPCImpersonationScope Dispatcher::BeginDPCImpersonation(X_KPCR& pcr) {
  DPCImpersonationScope scope;
  scope.previous_irql_ = pcr.current_irql;
  pcr.current_irql = IRQL_DISPATCH;
  pcr.prcb_data.dpc_active = 1;
  return scope;
}

void Dispatcher::EndDPCImpersonation(X_KPCR& pcr, const DPCImpersonationScope& scope) {
  pcr.prcb_data.dpc_active = 0;
  pcr.current_irql = scope.previous_irql_;
}

bool Dispatcher::QueueDPC(u32 dpc, u32 arg1, u32 arg2) {
  auto* memory = runtime_.memory();
  std::lock_guard lock(dpc_mutex_);
  auto* object = memory->TranslateVirtual<XDPC*>(dpc);
  if (object->selected_cpu_number) {
    return false;
  }
  object->selected_cpu_number = 1;
  object->arg1 = arg1;
  object->arg2 = arg2;
  util::XeInsertTailList(dpc_list_head_, dpc + kDPCListEntryOffset, memory);
  return true;
}

bool Dispatcher::RemoveDPC(u32 dpc) {
  auto* memory = runtime_.memory();
  std::lock_guard lock(dpc_mutex_);
  auto* object = memory->TranslateVirtual<XDPC*>(dpc);
  if (!object->selected_cpu_number) {
    return false;
  }
  util::XeRemoveEntryList(&object->list_entry, memory);
  object->selected_cpu_number = 0;
  return true;
}

bool Dispatcher::HasQueuedDPCs() {
  auto* memory = runtime_.memory();
  std::lock_guard lock(dpc_mutex_);
  return !util::XeIsListEmpty(memory->TranslateVirtual<X_LIST_ENTRY*>(dpc_list_head_), memory);
}

std::optional<Dispatcher::QueuedDPC> Dispatcher::PopDPC() {
  auto* memory = runtime_.memory();
  std::lock_guard lock(dpc_mutex_);
  auto* head = memory->TranslateVirtual<X_LIST_ENTRY*>(dpc_list_head_);
  if (util::XeIsListEmpty(head, memory)) {
    return std::nullopt;
  }
  const u32 address = util::XeRemoveHeadList(head, memory) - kDPCListEntryOffset;
  auto* object = memory->TranslateVirtual<XDPC*>(address);
  object->selected_cpu_number = 0;
  return QueuedDPC{address, object->routine, object->context, object->arg1, object->arg2};
}

void Dispatcher::RunQueuedDPCs(ThreadState& state) {
  auto* ctx = state.context();
  auto& pcr = *runtime_.memory()->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  auto* functions = runtime_.function_dispatcher();
  const DPCImpersonationScope scope = BeginDPCImpersonation(pcr);
  while (const std::optional<QueuedDPC> dpc = PopDPC()) {
    const u64 saved_r1 = ctx->r1.u64;
    ctx->r1.u64 = dpc_stack_top_;
    u64 args[] = {dpc->address, dpc->context, dpc->arg1, dpc->arg2};
    functions->ExecuteTrap(&state, dpc->routine, args, rex::countof(args));
    ctx->r1.u64 = saved_r1;
  }
  EndDPCImpersonation(pcr, scope);
}

void Dispatcher::DrainDPCs() {
  ThreadState* state = ThreadState::Get();
  if (!state || !state->context()->r13.u64 || !dpc_stack_top_ || t_interrupt_depth || t_draining) {
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

void Dispatcher::Post(std::function<void()> work) {
  {
    std::lock_guard lock(dispatch_mutex_);
    dispatch_queue_.push_back(std::move(work));
  }
  dispatch_cond_.notify_all();
}

void Dispatcher::PostDrain() {
  Post([this] { DrainDPCs(); });
}

bool Dispatcher::IsDispatchThread() const {
  return dispatch_thread_ && ThreadRecord::IsInThread() &&
         ThreadRecord::GetCurrentThread()->handle() == dispatch_thread_;
}

bool Dispatcher::IsRunning() const {
  return dispatch_thread_running_.load();
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
            AllocateGuestStack(*runtime_.memory(), kDPCStackSize, 0, false)) {
      dpc_stack_base_ = stack->alloc_base;
      dpc_stack_top_ = stack->base - kDPCStackTopReserve;
    } else {
      REXLOG_ERROR(system, "Unable to allocate the DPC stack");
    }
  }
  dispatch_thread_running_ = true;
  auto record = std::make_unique<ThreadRecord>(runtime_, 128 * 1024, 0, [this]() {
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
#if defined(__GLIBCXX__)
      } catch (const abi::__forced_unwind&) {
        throw;
#endif
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
  record->set_name("Kernel Dispatch");
  Result<X_HANDLE> launched = runtime_.thread_registry().Launch(std::move(record));
  if (!launched) {
    dispatch_thread_running_ = false;
    dispatch_thread_ = 0;
    REX_FATAL(system, "Failed to create kernel dispatch thread (status {:#x})",
              ToXStatus(launched));
  }
  dispatch_thread_ = *launched;
}

void Dispatcher::Stop(std::chrono::milliseconds timeout) {
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
  if (dispatch_thread_running_) {
    {
      std::lock_guard lock(dispatch_mutex_);
      dispatch_thread_running_ = false;
    }
    dispatch_cond_.notify_all();
    u64 wait = static_cast<u64>(-static_cast<i64>(timeout.count()) * 10000);
    if (WaitOn(runtime_, dispatch_thread_, 0, 0, 0, &wait) == X_STATUS_TIMEOUT) {
      REXLOG_WARN(system, "Kernel Dispatch thread still running after {} ms, terminating it",
                  timeout.count());
      if (ThreadPin thread = runtime_.thread_registry().FindByHandle(dispatch_thread_)) {
        thread->Terminate(0);
      }
    }
    (void)runtime_.object_table().Close(dispatch_thread_);
    dispatch_thread_ = 0;
    std::lock_guard lock(dispatch_mutex_);
    dispatch_queue_.clear();
  }
}

}  // namespace rex::runtime
