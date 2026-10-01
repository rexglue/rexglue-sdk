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

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xthread.h>

namespace rex::runtime {

namespace {

constexpr u32 kDeferredOverlappedDelayMillis = 100;

}  // namespace

Dispatcher::Dispatcher(Runtime& runtime) : runtime_(runtime), dpc_list_(runtime.memory()) {}

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

Dispatcher::DPCImpersonationScope Dispatcher::BeginDPCImpersonation() {
  auto* ctx = ThreadRecord::GetCurrentThread()->thread_state()->context();
  auto pcr = runtime_.memory()->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  DPCImpersonationScope scope;
  scope.previous_irql_ = pcr->current_irql;
  pcr->current_irql = IRQL_DISPATCH;
  pcr->prcb_data.dpc_active = 1;
  return scope;
}

void Dispatcher::EndDPCImpersonation(const DPCImpersonationScope& scope) {
  auto* ctx = ThreadRecord::GetCurrentThread()->thread_state()->context();
  auto pcr = runtime_.memory()->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  pcr->prcb_data.dpc_active = 0;
  pcr->current_irql = scope.previous_irql_;
}

void Dispatcher::Start() {
  if (dispatch_thread_running_) {
    return;
  }
  dispatch_thread_running_ = true;
  auto record = std::make_unique<ThreadRecord>(runtime_, 128 * 1024, 0, [this]() {
    std::unique_lock lock(dispatch_mutex_, std::defer_lock);
    while (dispatch_thread_running_) {
      lock.lock();
      if (dispatch_queue_.empty()) {
        dispatch_cond_.wait(lock);
        if (!dispatch_thread_running_) {
          lock.unlock();
          break;
        }
      }
      auto fn = std::move(dispatch_queue_.front());
      dispatch_queue_.pop_front();
      REXLOG_DEV(system, "Dispatch thread processing queued item ({} remaining)",
                 dispatch_queue_.size());
      lock.unlock();

      try {
        fn();
      } catch (const std::exception& e) {
        REX_FATAL(system, "Dispatch thread: deferred completion threw '{}'", e.what());
      } catch (...) {
        REX_FATAL(system, "Dispatch thread: deferred completion threw non-std exception");
      }
      REXLOG_DEV(system, "Dispatch thread completed item");
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

void Dispatcher::Stop() {
  if (!dispatch_thread_running_) {
    return;
  }
  dispatch_thread_running_ = false;
  dispatch_cond_.notify_all();
  WaitOn(runtime_, dispatch_thread_, 0, 0, 0, nullptr);
  (void)runtime_.object_table().Close(dispatch_thread_);
  dispatch_thread_ = 0;
}

}  // namespace rex::runtime
