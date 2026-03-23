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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xmutant.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>
#include <rex/system/xtypes.h>
#include <rex/thread/atomic.h>
#include <rex/thread/mutex.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;
using rex::runtime::current_ppc_context;

// r13 + 0x100: pointer to thread local state
// Thread local state:
//   0x058: kernel time
//   0x14C: thread id
//   0x150: if >0 then error states don't get set
//   0x160: last error

// GetCurrentThreadId:
// lwz       r11, 0x100(r13)
// lwz       r3, 0x14C(r11)

// RtlGetLastError:
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

// RtlSetLastError:
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

// RtlSetLastNTError:
// r3 = RtlNtStatusToDosError(r3)
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

template <typename T>
object_ref<T> LookupNamedObject(KernelState* kernel_state, uint32_t obj_attributes_ptr) {
  // If the name exists and its type matches, we can return that (ref+1)
  // with a success of NAME_EXISTS.
  // If the name exists and its type doesn't match, we do NAME_COLLISION.
  // Otherwise, we add like normal.
  if (!obj_attributes_ptr) {
    return nullptr;
  }
  auto obj_attributes =
      kernel_state->memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_state->memory(), obj_attributes->name_ptr);
  if (!name.empty()) {
    X_HANDLE handle = X_INVALID_HANDLE_VALUE;
    X_RESULT result = kernel_state->object_table()->GetObjectByName(name, &handle);
    if (XSUCCEEDED(result)) {
      // Found something! It's been retained, so return.
      auto obj = kernel_state->object_table()->LookupObject<T>(handle);
      if (obj) {
        // The caller will do as it likes.
        obj->ReleaseHandle();
        return obj;
      }
    }
  }
  return nullptr;
}

ppc_u32_result_t ExCreateThread_entry(ppc_pu32_t handle_ptr, ppc_u32_t stack_size,
                                      ppc_pu32_t thread_id_ptr, ppc_u32_t xapi_thread_startup,
                                      ppc_pvoid_t start_address, ppc_pvoid_t start_context,
                                      ppc_u32_t creation_flags) {
  REXKRNL_IMPORT_TRACE(
      "ExCreateThread", "stack={:#x} xapi_startup={:#x} start={:#x} context={:#x} flags={:#x}",
      (uint32_t)stack_size, (uint32_t)xapi_thread_startup, start_address.guest_address(),
      start_context.guest_address(), (uint32_t)creation_flags);
  // http://jafile.com/uploads/scoop/main.cpp.txt
  // DWORD
  // LPHANDLE Handle,
  // DWORD    StackSize,
  // LPDWORD  ThreadId,
  // LPVOID   XapiThreadStartup, ?? often 0
  // LPVOID   StartAddress,
  // LPVOID   StartContext,
  // DWORD    CreationFlags // 0x80?

  // Determine target process based on creation flags.
  uint32_t guest_process = REX_KERNEL_STATE()->GetTitleProcess();
  if (creation_flags & 2) {
    REXKRNL_WARN("[ExCreateThread] Guest is creating a system thread!");
    guest_process = REX_KERNEL_STATE()->GetSystemProcess();
  }

  // Inherit default stack size
  uint32_t actual_stack_size = stack_size;

  if (actual_stack_size == 0) {
    actual_stack_size = REX_KERNEL_STATE()->GetExecutableModule()->stack_size();
  }

  // Stack must be aligned to 16kb pages
  actual_stack_size = std::max((uint32_t)0x4000, ((actual_stack_size + 0xFFF) & 0xFFFFF000));

  auto thread = object_ref<XThread>(new XThread(
      REX_KERNEL_STATE(), actual_stack_size, xapi_thread_startup, start_address.guest_address(),
      start_context.guest_address(), creation_flags, true, false, guest_process));

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    // Failed!
    REXKRNL_ERROR("Thread creation failed: {:08X}", result);
    return result;
  }

  if (XSUCCEEDED(result)) {
    if (handle_ptr) {
      if (creation_flags & 0x80) {
        *handle_ptr = thread->guest_object();
      } else {
        *handle_ptr = thread->handle();
      }
    }
    if (thread_id_ptr) {
      *thread_id_ptr = thread->thread_id();
    }
  }
  REXKRNL_IMPORT_RESULT("ExCreateThread", "{:#x} handle={:#x} tid={}", result,
                        handle_ptr ? (uint32_t)*handle_ptr : 0,
                        thread_id_ptr ? (uint32_t)*thread_id_ptr : 0);
  return result;
}

ppc_u32_result_t ExTerminateThread_entry(ppc_u32_t exit_code) {
  XThread* thread = XThread::GetCurrentThread();

  // NOTE: this kills us right now. We won't return from it.
  return thread->Exit(exit_code);
}

ppc_u32_result_t NtResumeThread_entry(ppc_u32_t handle, ppc_pu32_t suspend_count_ptr) {
  X_RESULT result = X_STATUS_INVALID_HANDLE;
  uint32_t suspend_count = 0;

  auto thread = REX_KERNEL_OBJECTS()->LookupObject<XThread>(handle);
  if (thread) {
    REXKRNL_TRACE("[NtResumeThread] handle={:08X} thread={}", uint32_t(handle), thread->name());
    result = thread->Resume(&suspend_count);
  } else {
    REXKRNL_WARN("[NtResumeThread] handle={:08X} NOT FOUND", uint32_t(handle));
  }
  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  REXKRNL_TRACE("[NtResumeThread] -> {:#x} suspend_count={}", result, suspend_count);
  return result;
}

ppc_u32_result_t KeResumeThread_entry(ppc_pvoid_t thread_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  auto thread = XObject::GetNativeObject<XThread>(REX_KERNEL_STATE(), thread_ptr);
  if (thread) {
    REXKRNL_TRACE("[KeResumeThread] ptr={:08X} thread={}", thread_ptr.guest_address(),
                  thread->name());
    result = thread->Resume();
  } else {
    REXKRNL_WARN("[KeResumeThread] ptr={:08X} NOT FOUND", thread_ptr.guest_address());
    result = X_STATUS_INVALID_HANDLE;
  }

  REXKRNL_TRACE("[KeResumeThread] -> {:#x}", result);
  return result;
}

ppc_u32_result_t NtSuspendThread_entry(ppc_u32_t handle, ppc_pu32_t suspend_count_ptr) {
  X_RESULT result = X_STATUS_SUCCESS;
  uint32_t suspend_count = 0;

  auto thread = REX_KERNEL_OBJECTS()->LookupObject<XThread>(handle);
  if (thread) {
    if (thread->type() != XObject::Type::Thread) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }

    auto guest_thread = thread->guest_object<X_KTHREAD>();
    if (guest_thread->terminated) {
      return X_STATUS_THREAD_IS_TERMINATING;
    }

    REXKRNL_TRACE("[NtSuspendThread] handle={:08X} thread={}", uint32_t(handle), thread->name());
#if REX_PLATFORM_LINUX
    auto* current_thread = XThread::GetCurrentThread();
    bool is_self_suspend = current_thread && current_thread == thread.get();
    if (is_self_suspend) {
      REXKRNL_TRACE("[NtSuspendThread] self-suspend for thread={:08X}", uint32_t(handle));
      suspend_count = thread->SelfSuspend();
      result = X_STATUS_SUCCESS;
    } else {
      result = thread->Suspend(&suspend_count);
    }
#else
    result = thread->Suspend(&suspend_count);
#endif
  } else {
    REXKRNL_WARN("[NtSuspendThread] handle={:08X} NOT FOUND", uint32_t(handle));
    result = X_STATUS_INVALID_HANDLE;
  }

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  REXKRNL_TRACE("[NtSuspendThread] -> {:#x} suspend_count={}", result, suspend_count);
  return result;
}

namespace {

void UpdateGuestStackPointers(X_KTHREAD* kthread, X_KPCR* pcr, PPCContext* ctx, uint32_t sp,
                              uint32_t stack_alloc_base, uint32_t stack_base,
                              uint32_t stack_limit) {
  kthread->stack_alloc_base = stack_alloc_base;
  kthread->stack_base = stack_base;
  kthread->stack_limit = stack_limit;
  pcr->stack_base_ptr = stack_base;
  pcr->stack_end_ptr = stack_limit;
  ctx->r1.u64 = sp;
}

}  // namespace

void KeSetCurrentStackPointers_entry(ppc_pvoid_t stack_ptr, ppc_ptr_t<X_KTHREAD> thread,
                                     ppc_pvoid_t stack_alloc_base, ppc_pvoid_t stack_base,
                                     ppc_pvoid_t stack_limit) {
  auto current_thread = XThread::GetCurrentThread();
  auto context = current_thread->thread_state()->context();
  auto pcr =
      REX_KERNEL_MEMORY()->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(context->r13.u64));

  UpdateGuestStackPointers(thread, pcr, context, stack_ptr.guest_address(),
                           stack_alloc_base.value(), stack_base.value(), stack_limit.value());
}

ppc_u32_result_t KeSetAffinityThread_entry(ppc_pvoid_t thread_ptr, ppc_u32_t affinity,
                                           ppc_pu32_t previous_affinity_ptr) {
  REXKRNL_IMPORT_TRACE("KeSetAffinityThread", "thread={:#x} affinity={:#x}",
                       thread_ptr.guest_address(), (uint32_t)affinity);
  // The Xbox 360, according to disassembly of KeSetAffinityThread, unlike
  // Windows NT, stores the previous affinity via the pointer provided as an
  // argument, not in the return value - the return value is used for the
  // result.
  if (!affinity) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto thread = XObject::GetNativeObject<XThread>(REX_KERNEL_STATE(), thread_ptr);
  if (!thread) {
    REXKRNL_IMPORT_WARN("KeSetAffinityThread", "invalid thread handle ptr={:#x}",
                        thread_ptr.guest_address());
    return X_STATUS_INVALID_HANDLE;
  }
  if (previous_affinity_ptr) {
    *previous_affinity_ptr = uint32_t(1) << thread->active_cpu();
  }
  thread->SetAffinity(affinity);
  REXKRNL_IMPORT_RESULT("KeSetAffinityThread", "0x0");
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t KeQueryBasePriorityThread_entry(ppc_pvoid_t thread_ptr) {
  REXKRNL_IMPORT_TRACE("KeQueryBasePriorityThread", "thread={:#x}", thread_ptr.guest_address());
  int32_t priority = 0;

  auto thread = XObject::GetNativeObject<XThread>(REX_KERNEL_STATE(), thread_ptr);
  if (thread) {
    priority = thread->QueryPriority();
  }

  REXKRNL_IMPORT_RESULT("KeQueryBasePriorityThread", "{}", priority);
  return priority;
}

ppc_u32_result_t KeSetBasePriorityThread_entry(ppc_pvoid_t thread_ptr, ppc_u32_t increment) {
  REXKRNL_IMPORT_TRACE("KeSetBasePriorityThread", "thread={:#x} increment={}",
                       thread_ptr.guest_address(), (int32_t)increment);
  int32_t prev_priority = 0;
  auto thread = XObject::GetNativeObject<XThread>(REX_KERNEL_STATE(), thread_ptr);

  if (thread) {
    prev_priority = thread->QueryPriority();
    thread->SetPriority(increment);
  }
  REXKRNL_IMPORT_RESULT("KeSetBasePriorityThread", "prev_priority={}", prev_priority);

  return prev_priority;
}

ppc_u32_result_t KeSetDisableBoostThread_entry(ppc_ptr_t<X_KTHREAD> thread_ptr,
                                               ppc_u32_t disabled) {
  auto old_boost_disabled = reinterpret_cast<std::atomic_uint8_t*>(&thread_ptr->boost_disabled)
                                ->exchange(static_cast<uint8_t>(disabled));
  return old_boost_disabled;
}

ppc_u32_result_t KeGetCurrentProcessType_entry() {
  auto current_thread = XThread::GetCurrentThread();
  auto context = current_thread->thread_state()->context();
  auto pcr =
      REX_KERNEL_MEMORY()->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(context->r13.u64));

  if (pcr->prcb_data.dpc_active) {
    return pcr->processtype_value_in_dpc;
  }

  auto thread = REX_KERNEL_MEMORY()->TranslateVirtual<X_KTHREAD*>(pcr->prcb_data.current_thread);
  return thread->process_type;
}

void KeSetCurrentProcessType_entry(ppc_u32_t type) {
  // One of X_PROCTYPE_?
  assert_true(type <= 2);

  REX_KERNEL_STATE()->set_process_type(type);

  auto current_thread = XThread::GetCurrentThread();
  auto context = current_thread->thread_state()->context();
  auto pcr =
      REX_KERNEL_MEMORY()->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(context->r13.u64));

  if (pcr->prcb_data.dpc_active) {
    pcr->processtype_value_in_dpc = static_cast<uint8_t>(type);
  }
}

ppc_u32_result_t KeQueryPerformanceFrequency_entry() {
  uint64_t result = chrono::Clock::guest_tick_frequency();
  return static_cast<uint32_t>(result);
}

ppc_u32_result_t KeDelayExecutionThread_entry(ppc_u32_t processor_mode, ppc_u32_t alertable,
                                              ppc_pu64_t interval_ptr) {
  XThread* thread = XThread::GetCurrentThread();

  if (alertable) {
    thread->DeliverAPCs();
  }

  X_STATUS result = thread->Delay(processor_mode, alertable, *interval_ptr);

  if (alertable && result == X_STATUS_USER_APC) {
    thread->DeliverAPCs();
  }

  return result;
}

ppc_u32_result_t NtYieldExecution_entry() {
  rex::thread::MaybeYield();
  return X_STATUS_SUCCESS;
}

void KeQuerySystemTime_entry(ppc_pu64_t time_ptr) {
  uint64_t time = chrono::Clock::QueryGuestSystemTime();
  if (time_ptr) {
    *time_ptr = time;
  }
}

// https://msdn.microsoft.com/en-us/library/ms686801
ppc_u32_result_t KeTlsAlloc_entry() {
  uint32_t slot = REX_KERNEL_STATE()->AllocateTLS(current_ppc_context());
  if (slot != X_TLS_OUT_OF_INDEXES) {
    XThread::GetCurrentThread()->SetTLSValue(slot, 0);
  }
  REXKRNL_IMPORT_RESULT("KeTlsAlloc", "slot={}", slot);
  return slot;
}

// https://msdn.microsoft.com/en-us/library/ms686804
ppc_u32_result_t KeTlsFree_entry(ppc_u32_t tls_index) {
  REXKRNL_IMPORT_TRACE("KeTlsFree", "slot={}", (uint32_t)tls_index);
  if (tls_index == X_TLS_OUT_OF_INDEXES) {
    REXKRNL_IMPORT_RESULT("KeTlsFree", "0 (invalid)");
    return 0;
  }

  REX_KERNEL_STATE()->FreeTLS(current_ppc_context(), tls_index);
  REXKRNL_IMPORT_RESULT("KeTlsFree", "1");
  return 1;
}

// https://msdn.microsoft.com/en-us/library/ms686812
ppc_u32_result_t KeTlsGetValue_entry(ppc_u32_t tls_index) {
  // xboxkrnl doesn't actually have an error branch - it always succeeds, even
  // if it overflows the TLS.
  uint32_t value = 0;
  if (XThread::GetCurrentThread()->GetTLSValue(tls_index, &value)) {
    return value;
  }

  return 0;
}

// https://msdn.microsoft.com/en-us/library/ms686818
ppc_u32_result_t KeTlsSetValue_entry(ppc_u32_t tls_index, ppc_u32_t tls_value) {
  REXKRNL_IMPORT_TRACE("KeTlsSetValue", "slot={} value={:#x}", (uint32_t)tls_index,
                       (uint32_t)tls_value);
  // xboxkrnl doesn't actually have an error branch - it always succeeds, even
  // if it overflows the TLS.
  if (XThread::GetCurrentThread()->SetTLSValue(tls_index, tls_value)) {
    REXKRNL_IMPORT_RESULT("KeTlsSetValue", "1");
    return 1;
  }

  REXKRNL_IMPORT_RESULT("KeTlsSetValue", "0");
  return 0;
}

void KeInitializeEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, ppc_u32_t event_type,
                             ppc_u32_t initial_state) {
  event_ptr.Zero();
  event_ptr->header.type = event_type;
  event_ptr->header.signal_state = (uint32_t)initial_state;
  auto ev = XObject::GetNativeObject<XEvent>(REX_KERNEL_STATE(), event_ptr, event_type);
  if (!ev) {
    assert_always();
    return;
  }
}

uint32_t xeKeSetEvent(X_KEVENT* event_ptr, uint32_t increment, uint32_t wait) {
  auto ev = XObject::GetNativeObject<XEvent>(REX_KERNEL_STATE(), event_ptr);
  if (!ev) {
    assert_always();
    return 0;
  }

  return ev->Set(increment, !!wait);
}

ppc_u32_result_t KeSetEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, ppc_u32_t increment,
                                  ppc_u32_t wait) {
  return xeKeSetEvent(event_ptr, increment, wait);
}

ppc_u32_result_t KePulseEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, ppc_u32_t increment,
                                    ppc_u32_t wait) {
  auto ev = XObject::GetNativeObject<XEvent>(REX_KERNEL_STATE(), event_ptr);
  if (!ev) {
    assert_always();
    return 0;
  }

  return ev->Pulse(increment, !!wait);
}

ppc_u32_result_t KeResetEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr) {
  auto ev = XObject::GetNativeObject<XEvent>(REX_KERNEL_STATE(), event_ptr);
  if (!ev) {
    assert_always();
    return 0;
  }

  return ev->Reset();
}

ppc_u32_result_t NtCreateEvent_entry(ppc_pu32_t handle_ptr,
                                     ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
                                     ppc_u32_t event_type, ppc_u32_t initial_state) {
  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XEvent>(REX_KERNEL_STATE(), obj_attributes_ptr.guest_address());
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Event) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto ev = object_ref<XEvent>(new XEvent(REX_KERNEL_STATE()));
  ev->Initialize(!event_type, !!initial_state);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    ev->SetAttributes(obj_attributes_ptr.guest_address());
  }

  if (handle_ptr) {
    *handle_ptr = ev->handle();
  }
  return X_STATUS_SUCCESS;
}

uint32_t xeNtSetEvent(uint32_t handle, rex::be<uint32_t>* previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(handle);
  if (ev) {
    int32_t was_signalled = ev->Set(0, false);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

ppc_u32_result_t NtSetEvent_entry(ppc_u32_t handle, ppc_pu32_t previous_state_ptr) {
  return xeNtSetEvent(handle, previous_state_ptr);
}

ppc_u32_result_t NtPulseEvent_entry(ppc_u32_t handle, ppc_pu32_t previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(handle);
  if (ev) {
    int32_t was_signalled = ev->Pulse(0, false);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

uint32_t xeNtClearEvent(uint32_t handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Reset();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

ppc_u32_result_t NtClearEvent_entry(ppc_u32_t handle) {
  return xeNtClearEvent(handle);
}

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff552150(v=vs.85).aspx
void KeInitializeSemaphore_entry(ppc_ptr_t<X_KSEMAPHORE> semaphore_ptr, ppc_u32_t count,
                                 ppc_u32_t limit) {
  semaphore_ptr->header.type = 5;  // SemaphoreObject
  semaphore_ptr->header.signal_state = (uint32_t)count;
  semaphore_ptr->limit = (uint32_t)limit;

  auto sem = XObject::GetNativeObject<XSemaphore>(REX_KERNEL_STATE(), semaphore_ptr,
                                                  5 /* SemaphoreObject */);
  if (!sem) {
    assert_always();
    return;
  }
}

uint32_t xeKeReleaseSemaphore(X_KSEMAPHORE* semaphore_ptr, uint32_t increment, uint32_t adjustment,
                              uint32_t wait) {
  auto sem = XObject::GetNativeObject<XSemaphore>(REX_KERNEL_STATE(), semaphore_ptr);
  if (!sem) {
    assert_always();
    return 0;
  }

  // TODO(benvanik): increment thread priority?
  // TODO(benvanik): wait?

  int32_t previous_count = 0;
  [[maybe_unused]] bool success =
      sem->ReleaseSemaphore(static_cast<int32_t>(adjustment), &previous_count);
  return static_cast<uint32_t>(previous_count);
}

ppc_u32_result_t KeReleaseSemaphore_entry(ppc_ptr_t<X_KSEMAPHORE> semaphore_ptr,
                                          ppc_u32_t increment, ppc_u32_t adjustment,
                                          ppc_u32_t wait) {
  return xeKeReleaseSemaphore(semaphore_ptr, increment, adjustment, wait);
}

ppc_u32_result_t NtCreateSemaphore_entry(ppc_pu32_t handle_ptr, ppc_pvoid_t obj_attributes_ptr,
                                         ppc_u32_t count, ppc_u32_t limit) {
  // Check for an existing semaphore with the same name.
  auto existing_object =
      LookupNamedObject<XSemaphore>(REX_KERNEL_STATE(), obj_attributes_ptr.guest_address());
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Semaphore) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto sem = object_ref<XSemaphore>(new XSemaphore(REX_KERNEL_STATE()));
  if (!sem->Initialize((int32_t)count, (int32_t)limit)) {
    if (handle_ptr) {
      *handle_ptr = 0;
    }
    sem->ReleaseHandle();
    return X_STATUS_INVALID_PARAMETER;
  }

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    sem->SetAttributes(obj_attributes_ptr.guest_address());
  }

  if (handle_ptr) {
    *handle_ptr = sem->handle();
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t NtReleaseSemaphore_entry(ppc_u32_t sem_handle, ppc_u32_t release_count,
                                          ppc_pu32_t previous_count_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  int32_t previous_count = 0;

  auto sem = REX_KERNEL_OBJECTS()->LookupObject<XSemaphore>(sem_handle);
  if (sem) {
    bool success = sem->ReleaseSemaphore(static_cast<int32_t>(release_count), &previous_count);
    if (!success) {
      result = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (previous_count_ptr) {
    *previous_count_ptr = (uint32_t)previous_count;
  }

  return result;
}

ppc_u32_result_t NtCreateMutant_entry(ppc_pu32_t handle_out,
                                      ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attributes,
                                      ppc_u32_t initial_owner) {
  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XMutant>(REX_KERNEL_STATE(), obj_attributes.guest_address());
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Mutant) {
      if (handle_out) {
        existing_object->RetainHandle();
        *handle_out = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto mutant = object_ref<XMutant>(new XMutant(REX_KERNEL_STATE()));
  mutant->Initialize(initial_owner ? true : false);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes) {
    mutant->SetAttributes(obj_attributes.guest_address());
  }

  if (handle_out) {
    *handle_out = mutant->handle();
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t NtReleaseMutant_entry(ppc_u32_t mutant_handle, ppc_u32_t unknown) {
  // This doesn't seem to be supported.
  // int32_t previous_count_ptr = SHIM_GET_ARG_32(2);

  // Whatever arg 1 is all games seem to set it to 0, so whether it's
  // abandon or wait we just say false. Which is good, cause they are
  // both ignored.
  assert_zero(unknown);
  uint32_t priority_increment = 0;
  bool abandon = false;
  bool wait = false;

  X_STATUS result = X_STATUS_SUCCESS;

  auto mutant = REX_KERNEL_OBJECTS()->LookupObject<XMutant>(mutant_handle);
  if (mutant) {
    result = mutant->ReleaseMutant(priority_increment, abandon, wait);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

ppc_u32_result_t NtCreateTimer_entry(ppc_pu32_t handle_ptr, ppc_pvoid_t obj_attributes_ptr,
                                     ppc_u32_t timer_type) {
  // timer_type = NotificationTimer (0) or SynchronizationTimer (1)

  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XTimer>(REX_KERNEL_STATE(), obj_attributes_ptr.guest_address());
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Timer) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto timer = object_ref<XTimer>(new XTimer(REX_KERNEL_STATE()));
  timer->Initialize(timer_type);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    timer->SetAttributes(obj_attributes_ptr.guest_address());
  }

  if (handle_ptr) {
    *handle_ptr = timer->handle();
  }

  return X_STATUS_SUCCESS;
}

void KeInitializeTimerEx_entry(ppc_ptr_t<X_KTIMER> timer_ptr, ppc_u32_t timer_type,
                               ppc_u32_t proc_type) {
  assert_true(timer_type == 0 || timer_type == 1);
  assert_true(proc_type < 3);
  // Other fields are unmodified; they must carry through multiple calls.
  timer_ptr->header.process_type = static_cast<uint8_t>(proc_type & 0xFF);
  timer_ptr->header.inserted = 0;
  timer_ptr->header.type = static_cast<uint8_t>(timer_type + 8);
  timer_ptr->header.signal_state = 0;
  // Initialize wait list to point to itself (empty list).
  uint32_t wait_list_addr =
      timer_ptr.guest_address() + offsetof(X_DISPATCH_HEADER, wait_list_flink);
  timer_ptr->header.wait_list_flink = wait_list_addr;
  timer_ptr->header.wait_list_blink = wait_list_addr;
  timer_ptr->due_time = 0;
  timer_ptr->period = 0;
}

ppc_u32_result_t NtSetTimerEx_entry(ppc_u32_t timer_handle, ppc_pu64_t due_time_ptr,
                                    ppc_pvoid_t routine_ptr /*PTIMERAPCROUTINE*/, ppc_u32_t unk_one,
                                    ppc_pvoid_t routine_arg, ppc_u32_t resume, ppc_u32_t period_ms,
                                    ppc_u32_t unk_zero) {
  assert_true(unk_one == 1);
  assert_true(unk_zero == 0);

  uint64_t due_time = *due_time_ptr;

  X_STATUS result = X_STATUS_SUCCESS;

  auto timer = REX_KERNEL_OBJECTS()->LookupObject<XTimer>(timer_handle);
  if (timer) {
    result = timer->SetTimer(due_time, period_ms, routine_ptr.guest_address(),
                             routine_arg.guest_address(), resume ? true : false);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

ppc_u32_result_t NtCancelTimer_entry(ppc_u32_t timer_handle, ppc_pu32_t current_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto timer = REX_KERNEL_OBJECTS()->LookupObject<XTimer>(timer_handle);
  if (timer) {
    result = timer->Cancel();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (current_state_ptr) {
    *current_state_ptr = 0;
  }

  return result;
}

uint32_t xeKeWaitForSingleObject(void* object_ptr, uint32_t wait_reason, uint32_t processor_mode,
                                 uint32_t alertable, uint64_t* timeout_ptr) {
  auto object = XObject::GetNativeObject<XObject>(REX_KERNEL_STATE(), object_ptr);

  if (!object) {
    // The only kind-of failure code (though this should never happen)
    assert_always();
    return X_STATUS_ABANDONED_WAIT_0;
  }

  X_STATUS result = object->Wait(wait_reason, processor_mode, alertable, timeout_ptr);

  if (alertable && result == X_STATUS_USER_APC) {
    XThread::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

ppc_u32_result_t KeWaitForSingleObject_entry(ppc_pvoid_t object_ptr, ppc_u32_t wait_reason,
                                             ppc_u32_t processor_mode, ppc_u32_t alertable,
                                             ppc_pu64_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  // REXKRNL_IMPORT_TRACE("KeWaitForSingleObject", "obj={:#x} reason={} mode={} alertable={}
  // timeout={}",
  // object_ptr.guest_address(), (uint32_t)wait_reason,
  //(uint32_t)processor_mode, (uint32_t)alertable,
  // timeout_ptr ? (int64_t)timeout : -1);
  auto result = xeKeWaitForSingleObject(object_ptr, wait_reason, processor_mode, alertable,
                                        timeout_ptr ? &timeout : nullptr);
  // REXKRNL_IMPORT_RESULT("KeWaitForSingleObject", "{:#x}", result);
  return result;
}

ppc_u32_result_t NtWaitForSingleObjectEx_entry(ppc_u32_t object_handle, ppc_u32_t wait_mode,
                                               ppc_u32_t alertable, ppc_pu64_t timeout_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(object_handle);
  if (object) {
    uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
    result = object->Wait(3, wait_mode, alertable, timeout_ptr ? &timeout : nullptr);
    if (alertable && result == X_STATUS_USER_APC) {
      XThread::GetCurrentThread()->DeliverAPCs();
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

ppc_u32_result_t KeWaitForMultipleObjects_entry(ppc_u32_t count, ppc_pu32_t objects_ptr,
                                                ppc_u32_t wait_type, ppc_u32_t wait_reason,
                                                ppc_u32_t processor_mode, ppc_u32_t alertable,
                                                ppc_pu64_t timeout_ptr,
                                                ppc_pvoid_t wait_block_array_ptr) {
  assert_true(wait_type <= 1);

  std::vector<object_ref<XObject>> objects;
  for (uint32_t n = 0; n < count; n++) {
    auto object_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(objects_ptr[n]);
    auto object_ref = XObject::GetNativeObject<XObject>(REX_KERNEL_STATE(), object_ptr);
    if (!object_ref) {
      return X_STATUS_INVALID_PARAMETER;
    }

    objects.push_back(std::move(object_ref));
  }

  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = XObject::WaitMultiple(
      uint32_t(objects.size()), reinterpret_cast<XObject**>(objects.data()), wait_type, wait_reason,
      processor_mode, alertable, timeout_ptr ? &timeout : nullptr);
  if (alertable && result == X_STATUS_USER_APC) {
    XThread::GetCurrentThread()->DeliverAPCs();
  }
  return result;
}

uint32_t xeNtWaitForMultipleObjectsEx(uint32_t count, rex::be<uint32_t>* handles,
                                      uint32_t wait_type, uint32_t wait_mode, uint32_t alertable,
                                      uint64_t* timeout_ptr) {
  assert_true(wait_type <= 1);

  std::vector<object_ref<XObject>> objects;
  for (uint32_t n = 0; n < count; n++) {
    uint32_t object_handle = handles[n];
    auto object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(object_handle);
    if (!object) {
      return X_STATUS_INVALID_PARAMETER;
    }
    objects.push_back(std::move(object));
  }

  auto result = XObject::WaitMultiple(count, reinterpret_cast<XObject**>(objects.data()), wait_type,
                                      6, wait_mode, alertable, timeout_ptr);
  if (alertable && result == X_STATUS_USER_APC) {
    XThread::GetCurrentThread()->DeliverAPCs();
  }
  return result;
}

ppc_u32_result_t NtWaitForMultipleObjectsEx_entry(ppc_u32_t count, ppc_pu32_t handles,
                                                  ppc_u32_t wait_type, ppc_u32_t wait_mode,
                                                  ppc_u32_t alertable, ppc_pu64_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  return xeNtWaitForMultipleObjectsEx(count, handles, wait_type, wait_mode, alertable,
                                      timeout_ptr ? &timeout : nullptr);
}

ppc_u32_result_t NtSignalAndWaitForSingleObjectEx_entry(ppc_u32_t signal_handle,
                                                        ppc_u32_t wait_handle, ppc_u32_t alertable,
                                                        ppc_u32_t r6, ppc_pu64_t timeout_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto signal_object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(signal_handle);
  auto wait_object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(wait_handle);
  if (signal_object && wait_object) {
    uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
    result = XObject::SignalAndWait(signal_object.get(), wait_object.get(), 3, 1, alertable,
                                    timeout_ptr ? &timeout : nullptr);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (alertable && result == X_STATUS_USER_APC) {
    XThread::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

// Guest-memory IRQL helpers - read/write current_irql directly from PCR.
// Take PPCContext* explicitly so they work from any thread (including host threads
// during InitializeGuestObject, dispatch thread creation, etc.).
static unsigned char xeKfRaiseIrql(PPCContext* ctx, unsigned char new_irql) {
  auto* mem = ctx->kernel_state->memory();
  auto pcr = mem->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  uint8_t old_irql = pcr->current_irql;
  pcr->current_irql = new_irql;
  return old_irql;
}

static void xeKfLowerIrql(PPCContext* ctx, unsigned char new_irql) {
  auto* mem = ctx->kernel_state->memory();
  auto pcr = mem->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  pcr->current_irql = new_irql;
}

// Guest-memory spinlock helpers - store PCR address as owner (matching xenia).
// PPCContext* provides r13 (PCR address) without needing XThread::GetCurrentThread().
uint32_t xeKeKfAcquireSpinLock(PPCContext* ctx, X_KSPINLOCK* lock, bool change_irql) {
  uint32_t old_irql = change_irql ? xeKfRaiseIrql(ctx, IRQL_DISPATCH) : 0;
  uint32_t pcr_addr = static_cast<uint32_t>(ctx->r13.u64);
  assert_true(lock->prcb_of_owner != rex::byte_swap(pcr_addr));  // deadlock detection
  while (!rex::thread::atomic_cas(0u, rex::byte_swap(pcr_addr), &lock->prcb_of_owner.value)) {
    rex::thread::MaybeYield();
  }
  return old_irql;
}

void xeKeKfReleaseSpinLock(PPCContext* ctx, X_KSPINLOCK* lock, uint32_t old_irql,
                           bool change_irql) {
  assert_true(lock->prcb_of_owner == static_cast<uint32_t>(ctx->r13.u64));
  rex::thread::atomic_store_release(0u, &lock->prcb_of_owner.value);
  if (change_irql && old_irql < IRQL_DISPATCH) {
    xeKfLowerIrql(ctx, static_cast<unsigned char>(old_irql));
  }
}

// Guest-memory APC helpers
void xeKeInitializeApc(XAPC* apc, uint32_t thread_ptr, uint32_t kernel_routine,
                       uint32_t rundown_routine, uint32_t normal_routine, uint32_t apc_mode,
                       uint32_t normal_context) {
  apc->thread_ptr = thread_ptr;
  apc->kernel_routine = kernel_routine;
  apc->rundown_routine = rundown_routine;
  apc->normal_routine = normal_routine;
  apc->type = 18;  // ApcObject
  if (normal_routine) {
    apc->apc_mode = apc_mode;
    apc->normal_context = normal_context;
  } else {
    apc->apc_mode = 0;
    apc->normal_context = 0;
  }
  apc->enqueued = 0;
}

uint32_t xeKeInsertQueueApc(XAPC* apc, uint32_t arg1, uint32_t arg2, uint32_t priority_increment,
                            PPCContext* ctx) {
  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto* mem = ctx->kernel_state->memory();
  auto target_thread = mem->TranslateVirtual<X_KTHREAD*>(thread_guest_pointer);
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &target_thread->apc_lock);
  uint32_t result;
  if (!target_thread->may_queue_apcs || apc->enqueued) {
    result = 0;
  } else {
    apc->arg1 = arg1;
    apc->arg2 = arg2;

    auto& which_list = target_thread->apc_lists[apc->apc_mode];

    if (apc->normal_routine) {
      which_list.InsertTail(apc, mem);
    } else {
      // Kernel-mode APCs without normal_routine go before those with one.
      XAPC* insertion_pos = nullptr;
      for (auto&& sub_apc : which_list.IterateForward(mem)) {
        insertion_pos = &sub_apc;
        if (sub_apc.normal_routine) {
          break;
        }
      }
      if (!insertion_pos) {
        which_list.InsertHead(apc, mem);
      } else {
        util::XeInsertHeadList(insertion_pos->list_entry.blink_ptr, &apc->list_entry, mem);
      }
    }

    apc->enqueued = 1;
    result = 1;
  }
  xeKeKfReleaseSpinLock(ctx, &target_thread->apc_lock, old_irql);
  return result;
}

ppc_u32_result_t KfAcquireSpinLock_entry(ppc_pu32_t lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  return xeKeKfAcquireSpinLock(current_ppc_context(), lock);
}

void KfReleaseSpinLock_entry(ppc_pu32_t lock_ptr, ppc_u32_t old_irql) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  xeKeKfReleaseSpinLock(current_ppc_context(), lock, old_irql);
}

void KeAcquireSpinLockAtRaisedIrql_entry(ppc_pu32_t lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  xeKeKfAcquireSpinLock(current_ppc_context(), lock, false);
}

ppc_u32_result_t KeTryToAcquireSpinLockAtRaisedIrql_entry(ppc_pu32_t lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  auto* ctx = current_ppc_context();
  uint32_t pcr_addr = static_cast<uint32_t>(ctx->r13.u64);
  if (!rex::thread::atomic_cas(0u, rex::byte_swap(pcr_addr), &lock->prcb_of_owner.value)) {
    return 0;
  }
  return 1;
}

void KeReleaseSpinLockFromRaisedIrql_entry(ppc_pu32_t lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  xeKeKfReleaseSpinLock(current_ppc_context(), lock, 0, false);
}

void KeEnterCriticalRegion_entry() {
  XThread::GetCurrentThread()->EnterCriticalRegion();
}

void KeLeaveCriticalRegion_entry() {
  XThread::GetCurrentThread()->LeaveCriticalRegion();
}

ppc_u32_result_t KeRaiseIrqlToDpcLevel_entry() {
  return xeKfRaiseIrql(current_ppc_context(), IRQL_DISPATCH);
}

void KfLowerIrql_entry(ppc_u32_t old_value) {
  xeKfLowerIrql(current_ppc_context(),
                static_cast<unsigned char>(static_cast<uint32_t>(old_value)));
}

ppc_u32_result_t KfRaiseIrql_entry(ppc_u32_t new_irql) {
  return xeKfRaiseIrql(current_ppc_context(),
                       static_cast<unsigned char>(static_cast<uint32_t>(new_irql)));
}

void NtQueueApcThread_entry(ppc_u32_t thread_handle, ppc_pvoid_t apc_routine,
                            ppc_pvoid_t apc_routine_context, ppc_pvoid_t arg1, ppc_pvoid_t arg2) {
  auto mem = REX_KERNEL_MEMORY();
  auto thread = REX_KERNEL_OBJECTS()->LookupObject<XThread>(thread_handle);

  if (!thread) {
    REXKRNL_ERROR("NtQueueApcThread: Incorrect thread handle! Might cause crash");
    return;
  }

  if (!apc_routine) {
    REXKRNL_ERROR("NtQueueApcThread: Incorrect apc routine! Might cause crash");
    return;
  }

  uint32_t apc_ptr = mem->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    return;
  }
  XAPC* apc = mem->TranslateVirtual<XAPC*>(apc_ptr);
  xeKeInitializeApc(apc, thread->guest_object(), XAPC::kDummyKernelRoutine, 0,
                    apc_routine.guest_address(), 1 /* user apc mode */,
                    apc_routine_context.guest_address());

  if (!xeKeInsertQueueApc(apc, arg1.guest_address(), arg2.guest_address(), 0,
                          current_ppc_context())) {
    mem->SystemHeapFree(apc_ptr);
    return;
  }
  // Match Edge/Canary behavior: callback is only a wakeup hint.
  // APC delivery happens via alertable wait handling.
  thread->thread()->QueueUserCallback([]() {});
}

void KeInitializeApc_entry(ppc_ptr_t<XAPC> apc, ppc_pvoid_t thread_ptr, ppc_pvoid_t kernel_routine,
                           ppc_pvoid_t rundown_routine, ppc_pvoid_t normal_routine,
                           ppc_u32_t processor_mode, ppc_pvoid_t normal_context) {
  xeKeInitializeApc(apc.host_address(), thread_ptr.guest_address(), kernel_routine.guest_address(),
                    rundown_routine.guest_address(), normal_routine.guest_address(), processor_mode,
                    normal_context.guest_address());
}

ppc_u32_result_t KeInsertQueueApc_entry(ppc_ptr_t<XAPC> apc, ppc_pvoid_t arg1, ppc_pvoid_t arg2,
                                        ppc_u32_t priority_increment) {
  return xeKeInsertQueueApc(apc.host_address(), arg1.guest_address(), arg2.guest_address(),
                            priority_increment, current_ppc_context());
}

ppc_u32_result_t KeRemoveQueueApc_entry(ppc_ptr_t<XAPC> apc) {
  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto* ctx = current_ppc_context();
  auto mem = REX_KERNEL_MEMORY();
  auto target_thread = mem->TranslateVirtual<X_KTHREAD*>(thread_guest_pointer);
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &target_thread->apc_lock);

  bool result = false;
  if (apc->enqueued) {
    apc->enqueued = 0;
    util::XeRemoveEntryList(&apc->list_entry, mem);
    result = true;
  }

  xeKeKfReleaseSpinLock(ctx, &target_thread->apc_lock, old_irql);
  return result ? 1 : 0;
}

ppc_u32_result_t KiApcNormalRoutineNop_entry(ppc_u32_t unk0 /* output? */,
                                             ppc_u32_t unk1 /* 0x13 */) {
  return 0;
}

void KeInitializeDpc_entry(ppc_ptr_t<XDPC> dpc, ppc_pvoid_t routine, ppc_pvoid_t context) {
  dpc->Initialize(routine.guest_address(), context.guest_address());
}

ppc_u32_result_t KeInsertQueueDpc_entry(ppc_ptr_t<XDPC> dpc, ppc_u32_t arg1, ppc_u32_t arg2) {
  assert_always("DPC does not dispatch yet; going to hang!");

  uint32_t list_entry_ptr = dpc.guest_address() + 4;

  // Lock dispatcher.
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();
  auto dpc_list = REX_KERNEL_STATE()->dpc_list();

  // If already in a queue, abort.
  if (dpc_list->IsQueued(list_entry_ptr)) {
    return 0;
  }

  // Prep DPC.
  dpc->arg1 = (uint32_t)arg1;
  dpc->arg2 = (uint32_t)arg2;

  dpc_list->Insert(list_entry_ptr);

  return 1;
}

ppc_u32_result_t KeRemoveQueueDpc_entry(ppc_ptr_t<XDPC> dpc) {
  bool result = false;

  uint32_t list_entry_ptr = dpc.guest_address() + 4;

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();
  auto dpc_list = REX_KERNEL_STATE()->dpc_list();
  if (dpc_list->IsQueued(list_entry_ptr)) {
    dpc_list->Remove(list_entry_ptr);
    result = true;
  }

  return result ? 1 : 0;
}

// https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/51e4dfcaacfdbd1a9692272931a436371492f72d/import/OpenXDK/include/xboxkrnl/xboxkrnl.h#L1372
struct X_ERWLOCK {
  be<int32_t> lock_count;              // 0x0
  be<uint32_t> writers_waiting_count;  // 0x4
  be<uint32_t> readers_waiting_count;  // 0x8
  be<uint32_t> readers_entry_count;    // 0xC
  X_KEVENT writer_event;               // 0x10
  X_KSEMAPHORE reader_semaphore;       // 0x20
  X_KSPINLOCK spin_lock;               // 0x34
};
static_assert_size(X_ERWLOCK, 0x38);

void ExInitializeReadWriteLock_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  lock_ptr->lock_count = -1;
  lock_ptr->writers_waiting_count = 0;
  lock_ptr->readers_waiting_count = 0;
  lock_ptr->readers_entry_count = 0;
  // Create GuestPointers to struct members with correct guest addresses
  ppc_ptr_t<X_KEVENT> event_ptr(&lock_ptr->writer_event,
                                lock_ptr.guest_address() + offsetof(X_ERWLOCK, writer_event));
  ppc_ptr_t<X_KSEMAPHORE> sem_ptr(&lock_ptr->reader_semaphore,
                                  lock_ptr.guest_address() + offsetof(X_ERWLOCK, reader_semaphore));
  KeInitializeEvent_entry(event_ptr, 1, 0);
  KeInitializeSemaphore_entry(sem_ptr, 0, 0x7FFFFFFF);
  lock_ptr->spin_lock.prcb_of_owner = 0;
}

void ExAcquireReadWriteLockExclusive_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = current_ppc_context();
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count) {
    xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count++;

  xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
  xeKeWaitForSingleObject(&lock_ptr->writer_event, 7, 0, 0, nullptr);
}

ppc_u32_result_t ExTryToAcquireReadWriteLockExclusive_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = current_ppc_context();
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0) {
    lock_ptr->lock_count = 0;
    result = 1;
  } else {
    result = 0;
  }

  xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
  return result;
}

void ExAcquireReadWriteLockShared_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = current_ppc_context();
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count || (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->readers_entry_count++;
    xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->readers_waiting_count++;

  xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
  xeKeWaitForSingleObject(&lock_ptr->reader_semaphore, 7, 0, 0, nullptr);
}

ppc_u32_result_t ExTryToAcquireReadWriteLockShared_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = current_ppc_context();
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0 ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->lock_count++;
    lock_ptr->readers_entry_count++;
    result = 1;
  } else {
    result = 0;
  }

  xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
  return result;
}

void ExReleaseReadWriteLock_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = current_ppc_context();
  auto old_irql = xeKeKfAcquireSpinLock(ctx, &lock_ptr->spin_lock);

  int32_t lock_count = --lock_ptr->lock_count;

  if (lock_count < 0) {
    lock_ptr->readers_entry_count = 0;
    xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  if (!lock_ptr->readers_entry_count) {
    auto readers_waiting_count = lock_ptr->readers_waiting_count;
    if (readers_waiting_count) {
      lock_ptr->readers_waiting_count = 0;
      lock_ptr->readers_entry_count = readers_waiting_count;
      xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
      xeKeReleaseSemaphore(&lock_ptr->reader_semaphore, 1, readers_waiting_count, 0);
      return;
    }
  }

  auto readers_entry_count = --lock_ptr->readers_entry_count;
  if (readers_entry_count) {
    xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count--;
  xeKeKfReleaseSpinLock(ctx, &lock_ptr->spin_lock, old_irql);
  xeKeSetEvent(&lock_ptr->writer_event, 1, 0);
}

// NOTE: This function is very commonly inlined, and probably won't be called!
ppc_ptr_result_t InterlockedPushEntrySList_entry(ppc_ptr_t<X_SLIST_HEADER> plist_ptr,
                                                 ppc_ptr_t<X_SINGLE_LIST_ENTRY> entry) {
  assert_not_null(plist_ptr);
  assert_not_null(entry);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {0};
  uint32_t old_head = 0;
  do {
    old_hdr = *plist_ptr;
    new_hdr.depth = old_hdr.depth + 1;
    new_hdr.sequence = old_hdr.sequence + 1;

    old_head = old_hdr.next.next;
    entry->next = old_hdr.next.next;
    new_hdr.next.next = entry.guest_address();
  } while (!rex::thread::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                                    reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return old_head;
}

ppc_ptr_result_t InterlockedPopEntrySList_entry(ppc_ptr_t<X_SLIST_HEADER> plist_ptr) {
  assert_not_null(plist_ptr);

  uint32_t popped = 0;
  alignas(8) X_SLIST_HEADER old_hdr = {0};
  alignas(8) X_SLIST_HEADER new_hdr = {0};
  do {
    old_hdr = *plist_ptr;
    auto next = REX_KERNEL_MEMORY()->TranslateVirtual<X_SINGLE_LIST_ENTRY*>(old_hdr.next.next);
    if (!old_hdr.next.next) {
      return 0;
    }
    popped = old_hdr.next.next;

    new_hdr.depth = old_hdr.depth - 1;
    new_hdr.next.next = next->next;
    new_hdr.sequence = old_hdr.sequence;
  } while (!rex::thread::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                                    reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return popped;
}

ppc_ptr_result_t InterlockedFlushSList_entry(ppc_ptr_t<X_SLIST_HEADER> plist_ptr) {
  assert_not_null(plist_ptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {0};
  uint32_t first = 0;
  do {
    old_hdr = *plist_ptr;
    first = old_hdr.next.next;
    new_hdr.next.next = 0;
    new_hdr.depth = 0;
    new_hdr.sequence = 0;
  } while (!rex::thread::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                                    reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return first;
}

XBOXKRNL_EXPORT_STUB(__imp__KeInsertByKeyDeviceQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeInsertDeviceQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeInsertHeadQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeInsertQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeReleaseMutant);
XBOXKRNL_EXPORT_STUB(__imp__KeRemoveByKeyDeviceQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeRemoveDeviceQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeRemoveEntryDeviceQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeRemoveQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeSetEventBoostPriority);
XBOXKRNL_EXPORT_STUB(__imp__KiApcNormalRoutineNop_);

ppc_u32_result_t KeSuspendThread_entry(ppc_pvoid_t kthread_ptr) {
  auto thread = XObject::GetNativeObject<XThread>(REX_KERNEL_STATE(), kthread_ptr);
  uint32_t old_suspend_count = 0;

  if (thread) {
    old_suspend_count = thread->suspend_count();
    uint32_t discarded = 0;
    thread->Suspend(&discarded);
  }

  return old_suspend_count;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__ExCreateThread, rex::kernel::xboxkrnl::ExCreateThread_entry)
XBOXKRNL_EXPORT(__imp__ExTerminateThread, rex::kernel::xboxkrnl::ExTerminateThread_entry)
XBOXKRNL_EXPORT(__imp__NtResumeThread, rex::kernel::xboxkrnl::NtResumeThread_entry)
XBOXKRNL_EXPORT(__imp__KeResumeThread, rex::kernel::xboxkrnl::KeResumeThread_entry)
XBOXKRNL_EXPORT(__imp__NtSuspendThread, rex::kernel::xboxkrnl::NtSuspendThread_entry)
XBOXKRNL_EXPORT(__imp__KeSuspendThread, rex::kernel::xboxkrnl::KeSuspendThread_entry)
XBOXKRNL_EXPORT(__imp__KeSetCurrentStackPointers,
                rex::kernel::xboxkrnl::KeSetCurrentStackPointers_entry)
XBOXKRNL_EXPORT(__imp__KeSetAffinityThread, rex::kernel::xboxkrnl::KeSetAffinityThread_entry)
XBOXKRNL_EXPORT(__imp__KeQueryBasePriorityThread,
                rex::kernel::xboxkrnl::KeQueryBasePriorityThread_entry)
XBOXKRNL_EXPORT(__imp__KeSetBasePriorityThread,
                rex::kernel::xboxkrnl::KeSetBasePriorityThread_entry)
XBOXKRNL_EXPORT(__imp__KeSetDisableBoostThread,
                rex::kernel::xboxkrnl::KeSetDisableBoostThread_entry)
XBOXKRNL_EXPORT(__imp__KeGetCurrentProcessType,
                rex::kernel::xboxkrnl::KeGetCurrentProcessType_entry)
XBOXKRNL_EXPORT(__imp__KeSetCurrentProcessType,
                rex::kernel::xboxkrnl::KeSetCurrentProcessType_entry)
XBOXKRNL_EXPORT(__imp__KeQueryPerformanceFrequency,
                rex::kernel::xboxkrnl::KeQueryPerformanceFrequency_entry)
XBOXKRNL_EXPORT(__imp__KeDelayExecutionThread, rex::kernel::xboxkrnl::KeDelayExecutionThread_entry)
XBOXKRNL_EXPORT(__imp__NtYieldExecution, rex::kernel::xboxkrnl::NtYieldExecution_entry)
XBOXKRNL_EXPORT(__imp__KeQuerySystemTime, rex::kernel::xboxkrnl::KeQuerySystemTime_entry)
XBOXKRNL_EXPORT(__imp__KeTlsAlloc, rex::kernel::xboxkrnl::KeTlsAlloc_entry)
XBOXKRNL_EXPORT(__imp__KeTlsFree, rex::kernel::xboxkrnl::KeTlsFree_entry)
XBOXKRNL_EXPORT(__imp__KeTlsGetValue, rex::kernel::xboxkrnl::KeTlsGetValue_entry)
XBOXKRNL_EXPORT(__imp__KeTlsSetValue, rex::kernel::xboxkrnl::KeTlsSetValue_entry)
XBOXKRNL_EXPORT(__imp__KeInitializeEvent, rex::kernel::xboxkrnl::KeInitializeEvent_entry)
XBOXKRNL_EXPORT(__imp__KeSetEvent, rex::kernel::xboxkrnl::KeSetEvent_entry)
XBOXKRNL_EXPORT(__imp__KePulseEvent, rex::kernel::xboxkrnl::KePulseEvent_entry)
XBOXKRNL_EXPORT(__imp__KeResetEvent, rex::kernel::xboxkrnl::KeResetEvent_entry)
XBOXKRNL_EXPORT(__imp__NtCreateEvent, rex::kernel::xboxkrnl::NtCreateEvent_entry)
XBOXKRNL_EXPORT(__imp__NtSetEvent, rex::kernel::xboxkrnl::NtSetEvent_entry)
XBOXKRNL_EXPORT(__imp__NtPulseEvent, rex::kernel::xboxkrnl::NtPulseEvent_entry)
XBOXKRNL_EXPORT(__imp__NtClearEvent, rex::kernel::xboxkrnl::NtClearEvent_entry)
XBOXKRNL_EXPORT(__imp__KeInitializeSemaphore, rex::kernel::xboxkrnl::KeInitializeSemaphore_entry)
XBOXKRNL_EXPORT(__imp__KeReleaseSemaphore, rex::kernel::xboxkrnl::KeReleaseSemaphore_entry)
XBOXKRNL_EXPORT(__imp__NtCreateSemaphore, rex::kernel::xboxkrnl::NtCreateSemaphore_entry)
XBOXKRNL_EXPORT(__imp__NtReleaseSemaphore, rex::kernel::xboxkrnl::NtReleaseSemaphore_entry)
XBOXKRNL_EXPORT(__imp__NtCreateMutant, rex::kernel::xboxkrnl::NtCreateMutant_entry)
XBOXKRNL_EXPORT(__imp__NtReleaseMutant, rex::kernel::xboxkrnl::NtReleaseMutant_entry)
XBOXKRNL_EXPORT(__imp__NtCreateTimer, rex::kernel::xboxkrnl::NtCreateTimer_entry)
XBOXKRNL_EXPORT(__imp__NtSetTimerEx, rex::kernel::xboxkrnl::NtSetTimerEx_entry)
XBOXKRNL_EXPORT(__imp__NtCancelTimer, rex::kernel::xboxkrnl::NtCancelTimer_entry)
XBOXKRNL_EXPORT(__imp__KeInitializeTimerEx, rex::kernel::xboxkrnl::KeInitializeTimerEx_entry)
XBOXKRNL_EXPORT(__imp__KeWaitForSingleObject, rex::kernel::xboxkrnl::KeWaitForSingleObject_entry)
XBOXKRNL_EXPORT(__imp__NtWaitForSingleObjectEx,
                rex::kernel::xboxkrnl::NtWaitForSingleObjectEx_entry)
XBOXKRNL_EXPORT(__imp__KeWaitForMultipleObjects,
                rex::kernel::xboxkrnl::KeWaitForMultipleObjects_entry)
XBOXKRNL_EXPORT(__imp__NtWaitForMultipleObjectsEx,
                rex::kernel::xboxkrnl::NtWaitForMultipleObjectsEx_entry)
XBOXKRNL_EXPORT(__imp__NtSignalAndWaitForSingleObjectEx,
                rex::kernel::xboxkrnl::NtSignalAndWaitForSingleObjectEx_entry)
XBOXKRNL_EXPORT(__imp__KfAcquireSpinLock, rex::kernel::xboxkrnl::KfAcquireSpinLock_entry)
XBOXKRNL_EXPORT(__imp__KfReleaseSpinLock, rex::kernel::xboxkrnl::KfReleaseSpinLock_entry)
XBOXKRNL_EXPORT(__imp__KeAcquireSpinLockAtRaisedIrql,
                rex::kernel::xboxkrnl::KeAcquireSpinLockAtRaisedIrql_entry)
XBOXKRNL_EXPORT(__imp__KeTryToAcquireSpinLockAtRaisedIrql,
                rex::kernel::xboxkrnl::KeTryToAcquireSpinLockAtRaisedIrql_entry)
XBOXKRNL_EXPORT(__imp__KeReleaseSpinLockFromRaisedIrql,
                rex::kernel::xboxkrnl::KeReleaseSpinLockFromRaisedIrql_entry)
XBOXKRNL_EXPORT(__imp__KeEnterCriticalRegion, rex::kernel::xboxkrnl::KeEnterCriticalRegion_entry)
XBOXKRNL_EXPORT(__imp__KeLeaveCriticalRegion, rex::kernel::xboxkrnl::KeLeaveCriticalRegion_entry)
XBOXKRNL_EXPORT(__imp__KeRaiseIrqlToDpcLevel, rex::kernel::xboxkrnl::KeRaiseIrqlToDpcLevel_entry)
XBOXKRNL_EXPORT(__imp__KfLowerIrql, rex::kernel::xboxkrnl::KfLowerIrql_entry)
XBOXKRNL_EXPORT(__imp__NtQueueApcThread, rex::kernel::xboxkrnl::NtQueueApcThread_entry)
XBOXKRNL_EXPORT(__imp__KeInitializeApc, rex::kernel::xboxkrnl::KeInitializeApc_entry)
XBOXKRNL_EXPORT(__imp__KeInsertQueueApc, rex::kernel::xboxkrnl::KeInsertQueueApc_entry)
XBOXKRNL_EXPORT(__imp__KeRemoveQueueApc, rex::kernel::xboxkrnl::KeRemoveQueueApc_entry)
XBOXKRNL_EXPORT(__imp__KiApcNormalRoutineNop, rex::kernel::xboxkrnl::KiApcNormalRoutineNop_entry)
XBOXKRNL_EXPORT(__imp__KeInitializeDpc, rex::kernel::xboxkrnl::KeInitializeDpc_entry)
XBOXKRNL_EXPORT(__imp__KeInsertQueueDpc, rex::kernel::xboxkrnl::KeInsertQueueDpc_entry)
XBOXKRNL_EXPORT(__imp__KeRemoveQueueDpc, rex::kernel::xboxkrnl::KeRemoveQueueDpc_entry)
XBOXKRNL_EXPORT(__imp__ExInitializeReadWriteLock,
                rex::kernel::xboxkrnl::ExInitializeReadWriteLock_entry)
XBOXKRNL_EXPORT(__imp__ExAcquireReadWriteLockExclusive,
                rex::kernel::xboxkrnl::ExAcquireReadWriteLockExclusive_entry)
XBOXKRNL_EXPORT(__imp__ExTryToAcquireReadWriteLockExclusive,
                rex::kernel::xboxkrnl::ExTryToAcquireReadWriteLockExclusive_entry)
XBOXKRNL_EXPORT(__imp__ExAcquireReadWriteLockShared,
                rex::kernel::xboxkrnl::ExAcquireReadWriteLockShared_entry)
XBOXKRNL_EXPORT(__imp__ExTryToAcquireReadWriteLockShared,
                rex::kernel::xboxkrnl::ExTryToAcquireReadWriteLockShared_entry)
XBOXKRNL_EXPORT(__imp__ExReleaseReadWriteLock, rex::kernel::xboxkrnl::ExReleaseReadWriteLock_entry)
XBOXKRNL_EXPORT(__imp__InterlockedPushEntrySList,
                rex::kernel::xboxkrnl::InterlockedPushEntrySList_entry)
XBOXKRNL_EXPORT(__imp__InterlockedPopEntrySList,
                rex::kernel::xboxkrnl::InterlockedPopEntrySList_entry)
XBOXKRNL_EXPORT(__imp__InterlockedFlushSList, rex::kernel::xboxkrnl::InterlockedFlushSList_entry)

XBOXKRNL_EXPORT_STUB(__imp__KeAlertResumeThread);
XBOXKRNL_EXPORT_STUB(__imp__KeAlertThread);
XBOXKRNL_EXPORT_STUB(__imp__KeBoostPriorityThread);
XBOXKRNL_EXPORT_STUB(__imp__KeCancelTimer);
XBOXKRNL_EXPORT_STUB(__imp__KeConnectInterrupt);
XBOXKRNL_EXPORT_STUB(__imp__KeContextFromKframes);
XBOXKRNL_EXPORT_STUB(__imp__KeContextToKframes);
XBOXKRNL_EXPORT_STUB(__imp__KeDisconnectInterrupt);
XBOXKRNL_EXPORT_STUB(__imp__KeFlushCacheRange);
XBOXKRNL_EXPORT_STUB(__imp__KeFlushCurrentEntireTb);
XBOXKRNL_EXPORT_STUB(__imp__KeFlushEntireTb);
XBOXKRNL_EXPORT_STUB(__imp__KeFlushMultipleTb);
XBOXKRNL_EXPORT_STUB(__imp__KeFlushUserModeCurrentTb);
XBOXKRNL_EXPORT_STUB(__imp__KeFlushUserModeTb);
XBOXKRNL_EXPORT_STUB(__imp__KeInitializeDeviceQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeInitializeInterrupt);
XBOXKRNL_EXPORT_STUB(__imp__KeInitializeMutant);
XBOXKRNL_EXPORT_STUB(__imp__KeInitializeQueue);
// XBOXKRNL_EXPORT_STUB(__imp__KeInitializeTimerEx); -- implemented below
XBOXKRNL_EXPORT_STUB(__imp__KeIpiGenericCall);
XBOXKRNL_EXPORT_STUB(__imp__KeQueryBackgroundProcessors);
XBOXKRNL_EXPORT_STUB(__imp__KeQueryInterruptTime);
XBOXKRNL_EXPORT_STUB(__imp__KeRegisterDriverNotification);
XBOXKRNL_EXPORT_STUB(__imp__KeRestoreFloatingPointState);
XBOXKRNL_EXPORT_STUB(__imp__KeRestoreVectorUnitState);
XBOXKRNL_EXPORT_STUB(__imp__KeRetireDpcList);
XBOXKRNL_EXPORT_STUB(__imp__KeRundownQueue);
XBOXKRNL_EXPORT_STUB(__imp__KeSaveFloatingPointState);
XBOXKRNL_EXPORT_STUB(__imp__KeSaveVectorUnitState);
XBOXKRNL_EXPORT_STUB(__imp__KeSetBackgroundProcessors);
XBOXKRNL_EXPORT_STUB(__imp__KeSetPriorityClassThread);
XBOXKRNL_EXPORT_STUB(__imp__KeSetPriorityThread);
XBOXKRNL_EXPORT_STUB(__imp__KeSetTimer);
XBOXKRNL_EXPORT_STUB(__imp__KeSetTimerEx);
XBOXKRNL_EXPORT_STUB(__imp__KeStallExecutionProcessor);
XBOXKRNL_EXPORT_STUB(__imp__KeSweepDcacheRange);
XBOXKRNL_EXPORT_STUB(__imp__KeSweepIcacheRange);
XBOXKRNL_EXPORT_STUB(__imp__KeTestAlertThread);
XBOXKRNL_EXPORT(__imp__KfRaiseIrql, rex::kernel::xboxkrnl::KfRaiseIrql_entry)
