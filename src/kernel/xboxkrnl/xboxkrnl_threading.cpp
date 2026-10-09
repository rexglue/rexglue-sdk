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
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/util/string_utils.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xmutant.h>
#include <rex/runtime/xsemaphore.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtimer.h>
#include <rex/runtime/xtypes.h>
#include <rex/platform/atomic.h>
#include "kernel/kernel_macros.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

namespace {

ThreadPin FindThread(u32 thread_address) {
  const X_HANDLE handle =
      runtime::Current().object_table().ResolveGuestObject(thread_address, -1).value_or(0);
  return handle ? runtime::Current().thread_registry().FindByHandle(handle) : ThreadPin{};
}

void DrainDPCsBelowDispatch(Runtime& runtime) {
  auto* ctx = ThreadState::Get()->context();
  auto* pcr = runtime.memory()->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  if (pcr->current_irql < IRQL_DISPATCH) {
    runtime.dispatcher().DrainDPCs();
  }
}

void DropStash(X_DISPATCH_HEADER& header) {
  if (header.wait_list_flink == kXObjSignature) {
    (void)runtime::Current().object_table().Close(header.wait_list_blink);
  }
  header.wait_list_flink = 0;
  header.wait_list_blink = 0;
}

}  // namespace

// r13 + 0x100: pointer to thread local state
// Thread local state:
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

u32 ExCreateThread_entry(mapped_u32 handle_ptr, u32 stack_size, mapped_u32 thread_id_ptr,
                         u32 xapi_thread_startup, mapped_void start_address,
                         mapped_void start_context, u32 creation_flags) {
  REXLOG_TRACE(kernel,
               "ExCreateThread "
               "stack={:#x} xapi_startup={:#x} start={:#x} context={:#x} flags={:#x}",
               static_cast<uint32_t>(stack_size), static_cast<uint32_t>(xapi_thread_startup),
               start_address.guest_address(), start_context.guest_address(),
               static_cast<uint32_t>(creation_flags));
  auto& runtime = runtime::Current();
  uint32_t guest_process = runtime.GetTitleProcess();
  if (creation_flags & 2) {
    REXLOG_WARN(kernel, "ExCreateThread Guest is creating a system thread!");
    guest_process = runtime.GetSystemProcess();
  }

  auto record = std::make_unique<ThreadRecord>(
      runtime, stack_size, xapi_thread_startup, start_address.guest_address(),
      start_context.guest_address(), creation_flags, true, false, guest_process);
  ThreadRecord* raw = record.get();

  X_HANDLE handle = 0;
  auto launched = runtime.thread_registry().Launch(std::move(record));
  if (launched) {
    handle = *launched;
  }
  X_STATUS result = ToXStatus(launched);
  if (XFAILED(result)) {
    REX_FAIL(kernel, result, "Thread creation failed: {:08X}", result);
  }

  const uint32_t thread_id = raw->thread_id();
  if (handle_ptr) {
    if (creation_flags & 0x80) {
      auto thread = runtime.thread_registry().FindByHandle(handle);
      *handle_ptr = thread ? thread->guest_object() : 0;
    } else {
      *handle_ptr = handle;
    }
  }
  if (thread_id_ptr) {
    *thread_id_ptr = thread_id;
  }
  REXLOG_TRACE(kernel,
               "ExCreateThread -> "
               "{:#x} handle={:#x} tid={}",
               result, handle_ptr ? static_cast<uint32_t>(*handle_ptr) : 0,
               thread_id_ptr ? static_cast<uint32_t>(*thread_id_ptr) : 0);
  return result;
}

u32 ExTerminateThread_entry(u32 exit_code) {
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();

  thread->Exit(static_cast<int>(exit_code));
}

u32 NtResumeThread_entry(u32 handle, mapped_u32 suspend_count_ptr) {
  X_RESULT result = X_STATUS_INVALID_HANDLE;
  uint32_t suspend_count = 0;

  auto thread = runtime::Current().thread_registry().FindByHandle(handle);
  if (thread) {
    REXLOG_TRACE(kernel, "NtResumeThread handle={:08X} thread={}", static_cast<uint32_t>(handle),
                 thread->name());
    result = thread->Resume(&suspend_count);
  } else {
    REXLOG_WARN(kernel, "NtResumeThread handle={:08X} NOT FOUND", static_cast<uint32_t>(handle));
  }
  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  REXLOG_TRACE(kernel, "NtResumeThread -> {:#x} suspend_count={}", result, suspend_count);
  return result;
}

u32 KeResumeThread_entry(mapped_void thread_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  auto thread = FindThread(thread_ptr.guest_address());
  if (thread) {
    REXLOG_TRACE(kernel, "KeResumeThread ptr={:08X} thread={}", thread_ptr.guest_address(),
                 thread->name());
    result = thread->Resume();
  } else {
    REXLOG_WARN(kernel, "KeResumeThread ptr={:08X} NOT FOUND", thread_ptr.guest_address());
    result = X_STATUS_INVALID_HANDLE;
  }

  REXLOG_TRACE(kernel, "KeResumeThread -> {:#x}", result);
  return result;
}

u32 NtSuspendThread_entry(u32 handle, mapped_u32 suspend_count_ptr) {
  X_RESULT result = X_STATUS_SUCCESS;
  uint32_t suspend_count = 0;

  auto thread = runtime::Current().thread_registry().FindByHandle(handle);
  if (thread) {
    auto guest_thread = thread->guest_object<X_KTHREAD>();
    if (guest_thread->terminated) {
      return X_STATUS_THREAD_IS_TERMINATING;
    }

    REXLOG_TRACE(kernel, "NtSuspendThread handle={:08X} thread={}", static_cast<uint32_t>(handle),
                 thread->name());
    result = thread->Suspend(&suspend_count);
  } else {
    REXLOG_WARN(kernel, "NtSuspendThread handle={:08X} NOT FOUND", static_cast<uint32_t>(handle));
    result = X_STATUS_INVALID_HANDLE;
  }

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  REXLOG_TRACE(kernel, "NtSuspendThread -> {:#x} suspend_count={}", result, suspend_count);
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

void KeSetCurrentStackPointers_entry(mapped_void stack_ptr, ppc_ptr_t<X_KTHREAD> thread,
                                     mapped_void stack_alloc_base, mapped_void stack_base,
                                     mapped_void stack_limit) {
  auto current_thread = ThreadRecord::GetCurrentThread();
  auto context = current_thread->thread_state()->context();
  auto pcr = runtime::Current().memory()->TranslateVirtual<X_KPCR*>(
      static_cast<uint32_t>(context->r13.u64));

  UpdateGuestStackPointers(thread, pcr, context, stack_ptr.guest_address(),
                           stack_alloc_base.value(), stack_base.value(), stack_limit.value());
}

u32 KeSetAffinityThread_entry(mapped_void thread_ptr, u32 affinity,
                              mapped_u32 previous_affinity_ptr) {
  REXLOG_TRACE(kernel,
               "KeSetAffinityThread "
               "thread={:#x} affinity={:#x}",
               thread_ptr.guest_address(), static_cast<uint32_t>(affinity));
  // KeSetAffinityThread, unlike Windows NT, stores the previous affinity through the pointer
  // argument, and its return value is the result.
  REX_KERNEL_ARG(affinity);
  auto thread = FindThread(thread_ptr.guest_address());
  if (!thread) {
    REXLOG_DEV(kernel,
               "KeSetAffinityThread "
               "invalid thread handle ptr={:#x}",
               thread_ptr.guest_address());
    return X_STATUS_INVALID_HANDLE;
  }
  if (previous_affinity_ptr) {
    *previous_affinity_ptr = static_cast<uint32_t>(1) << thread->active_cpu();
  }
  thread->SetAffinity(affinity);
  REXLOG_TRACE(kernel,
               "KeSetAffinityThread -> "
               "0x0");
  return X_STATUS_SUCCESS;
}

u32 KeQueryBasePriorityThread_entry(mapped_void thread_ptr) {
  REXLOG_TRACE(kernel,
               "KeQueryBasePriorityThread "
               "thread={:#x}",
               thread_ptr.guest_address());
  int32_t priority = 0;

  auto thread = FindThread(thread_ptr.guest_address());
  if (thread) {
    priority = thread->QueryPriority();
  }

  REXLOG_TRACE(kernel,
               "KeQueryBasePriorityThread -> "
               "{}",
               priority);
  return priority;
}

u32 KeSetBasePriorityThread_entry(mapped_void thread_ptr, u32 increment) {
  REXLOG_TRACE(kernel,
               "KeSetBasePriorityThread "
               "thread={:#x} increment={}",
               thread_ptr.guest_address(), static_cast<int32_t>(increment));
  int32_t prev_priority = 0;
  auto thread = FindThread(thread_ptr.guest_address());

  if (thread) {
    prev_priority = thread->QueryPriority();
    thread->SetPriority(increment);
  }
  REXLOG_TRACE(kernel,
               "KeSetBasePriorityThread -> "
               "prev_priority={}",
               prev_priority);

  return prev_priority;
}

u32 KeSetDisableBoostThread_entry(ppc_ptr_t<X_KTHREAD> thread_ptr, u32 disabled) {
  auto old_boost_disabled = reinterpret_cast<std::atomic_uint8_t*>(&thread_ptr->boost_disabled)
                                ->exchange(static_cast<uint8_t>(disabled));
  return old_boost_disabled;
}

u32 KeGetCurrentProcessType_entry() {
  auto current_thread = ThreadRecord::GetCurrentThread();
  auto context = current_thread->thread_state()->context();
  auto pcr = runtime::Current().memory()->TranslateVirtual<X_KPCR*>(
      static_cast<uint32_t>(context->r13.u64));

  if (pcr->prcb_data.dpc_active) {
    return pcr->processtype_value_in_dpc;
  }

  auto thread =
      runtime::Current().memory()->TranslateVirtual<X_KTHREAD*>(pcr->prcb_data.current_thread);
  return thread->process_type;
}

void KeSetCurrentProcessType_entry(u32 type) {
  // One of X_PROCTYPE_?
  REX_ASSERT(type <= 2);

  runtime::Current().set_process_type(type);

  auto current_thread = ThreadRecord::GetCurrentThread();
  auto context = current_thread->thread_state()->context();
  auto pcr = runtime::Current().memory()->TranslateVirtual<X_KPCR*>(
      static_cast<uint32_t>(context->r13.u64));

  if (pcr->prcb_data.dpc_active) {
    pcr->processtype_value_in_dpc = static_cast<uint8_t>(type);
  }
}

u32 KeQueryPerformanceFrequency_entry() {
  uint64_t result = Clock::guest_tick_frequency();
  return static_cast<uint32_t>(result);
}

u32 KeDelayExecutionThread_entry(u32 processor_mode, u32 alertable, mapped_u64 interval_ptr) {
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();

  if (alertable) {
    thread->DeliverAPCs();
  }

  X_STATUS result = thread->Delay(processor_mode, alertable, *interval_ptr);

  if (alertable && result == X_STATUS_USER_APC) {
    thread->DeliverAPCs();
  }

  return result;
}

u32 NtYieldExecution_entry() {
  ThreadRecord::CheckStop();
  ThreadRecord::NoteKernelTime();
  rex::platform::MaybeYield();
  return X_STATUS_SUCCESS;
}

void KeQuerySystemTime_entry(mapped_u64 time_ptr) {
  uint64_t time = Clock::QueryGuestSystemTime();
  if (time_ptr) {
    *time_ptr = time;
  }
}

// https://msdn.microsoft.com/en-us/library/ms686801
u32 KeTlsAlloc_entry() {
  uint32_t slot = runtime::Current()
                      .thread_registry()
                      .AllocateTLS(ThreadState::Get()->context())
                      .value_or(X_TLS_OUT_OF_INDEXES);
  if (slot != X_TLS_OUT_OF_INDEXES) {
    ThreadRecord::GetCurrentThread()->SetTLSValue(slot, 0);
  }
  REXLOG_TRACE(kernel,
               "KeTlsAlloc -> "
               "slot={}",
               slot);
  return slot;
}

// https://msdn.microsoft.com/en-us/library/ms686804
u32 KeTlsFree_entry(u32 tls_index) {
  REXLOG_TRACE(kernel,
               "KeTlsFree "
               "slot={}",
               static_cast<uint32_t>(tls_index));
  if (tls_index == X_TLS_OUT_OF_INDEXES) {
    REXLOG_TRACE(kernel,
                 "KeTlsFree -> "
                 "0 (invalid)");
    return 0;
  }

  runtime::Current().thread_registry().FreeTLS(ThreadState::Get()->context(), tls_index);
  REXLOG_TRACE(kernel,
               "KeTlsFree -> "
               "1");
  return 1;
}

// https://msdn.microsoft.com/en-us/library/ms686812
u32 KeTlsGetValue_entry(u32 tls_index) {
  // xboxkrnl doesn't have an error branch - it always succeeds, even
  // if it overflows the TLS.
  uint32_t value = 0;
  if (ThreadRecord::GetCurrentThread()->GetTLSValue(tls_index, &value)) {
    return value;
  }

  return 0;
}

// https://msdn.microsoft.com/en-us/library/ms686818
u32 KeTlsSetValue_entry(u32 tls_index, u32 tls_value) {
  REXLOG_TRACE(kernel,
               "KeTlsSetValue "
               "slot={} value={:#x}",
               static_cast<uint32_t>(tls_index), static_cast<uint32_t>(tls_value));
  // xboxkrnl doesn't have an error branch - it always succeeds, even
  // if it overflows the TLS.
  if (ThreadRecord::GetCurrentThread()->SetTLSValue(tls_index, tls_value)) {
    REXLOG_TRACE(kernel,
                 "KeTlsSetValue -> "
                 "1");
    return 1;
  }

  REXLOG_TRACE(kernel,
               "KeTlsSetValue -> "
               "0");
  return 0;
}

void KeInitializeEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, u32 event_type, u32 initial_state) {
  auto& table = runtime::Current().object_table();
  if (event_ptr->header.wait_list_flink == kXObjSignature) {
    if (auto existing = table.Get<EventObject>(event_ptr->header.wait_list_blink);
        existing && existing->manual_reset == (event_type == 0)) {
      event_ptr->header.type = static_cast<u8>(event_type);
      event_ptr->header.signal_state = initial_state;
      initial_state ? EventSet(*existing) : EventReset(*existing);
      return;
    }
    DropStash(event_ptr->header);
  }
  event_ptr.Zero();
  event_ptr->header.type = static_cast<u8>(event_type);
  event_ptr->header.signal_state = initial_state;
  if (!table.ResolveGuest<EventObject>(event_ptr.guest_address(), event_type)) {
    REXLOG_WARN(kernel, "KeInitializeEvent({:08X}) could not create the event",
                event_ptr.guest_address());
  }
}

uint32_t KeSetEvent(u32 event_address, uint32_t increment, uint32_t wait) {
  auto ev = runtime::Current().object_table().ResolveGuest<EventObject>(event_address);
  if (!ev) {
    REX_ASSERT_ALWAYS();
    return 0;
  }

  return EventSet(*ev);
}

u32 KeSetEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, u32 increment, u32 wait) {
  return KeSetEvent(event_ptr.guest_address(), increment, wait);
}

u32 KePulseEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, u32 increment, u32 wait) {
  auto ev = runtime::Current().object_table().ResolveGuest<EventObject>(event_ptr.guest_address());
  if (!ev) {
    REX_ASSERT_ALWAYS();
    return 0;
  }

  return EventPulse(*ev);
}

u32 KeResetEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr) {
  auto ev = runtime::Current().object_table().ResolveGuest<EventObject>(event_ptr.guest_address());
  if (!ev) {
    REX_ASSERT_ALWAYS();
    return 0;
  }

  return EventReset(*ev);
}

u32 NtCreateEvent_entry(mapped_u32 handle_ptr, ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
                        u32 event_type, u32 initial_state) {
  REX_KERNEL_NAMED_CREATE(Event, handle_ptr, obj_attributes_ptr);

  X_HANDLE handle = 0;
  auto added = runtime::Current().object_table().CreateWithBlock<EventObject>(
      obj_attributes_ptr.guest_address(), sizeof(X_KEVENT), !event_type, !!initial_state);
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XFAILED(status)) {
    return status;
  }
  if (handle_ptr) {
    *handle_ptr = handle;
  }
  return X_STATUS_SUCCESS;
}

uint32_t NtSetEvent(uint32_t handle, be_u32* previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::Current().object_table().Get<EventObject>(handle);
  if (ev) {
    int32_t was_signalled = EventSet(*ev);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtSetEvent_entry(u32 handle, mapped_u32 previous_state_ptr) {
  return NtSetEvent(handle, previous_state_ptr);
}

u32 NtPulseEvent_entry(u32 handle, mapped_u32 previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::Current().object_table().Get<EventObject>(handle);
  if (ev) {
    int32_t was_signalled = EventPulse(*ev);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

uint32_t NtClearEvent(uint32_t handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::Current().object_table().Get<EventObject>(handle);
  if (ev) {
    EventReset(*ev);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtClearEvent_entry(u32 handle) {
  return NtClearEvent(handle);
}

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff552150(v=vs.85).aspx
void KeInitializeSemaphore_entry(ppc_ptr_t<X_KSEMAPHORE> semaphore_ptr, u32 count, u32 limit) {
  semaphore_ptr->header.type = 5;  // SemaphoreObject
  semaphore_ptr->header.signal_state = static_cast<uint32_t>(count);
  semaphore_ptr->limit = static_cast<uint32_t>(limit);

  auto sem = runtime::Current().object_table().ResolveGuest<SemaphoreObject>(
      semaphore_ptr.guest_address(), 5);
  if (!sem) {
    REX_ASSERT_ALWAYS();
    return;
  }
}

uint32_t KeReleaseSemaphore(u32 semaphore_address, uint32_t increment, uint32_t adjustment,
                            uint32_t wait) {
  auto sem = runtime::Current().object_table().ResolveGuest<SemaphoreObject>(semaphore_address);
  if (!sem) {
    REX_ASSERT_ALWAYS();
    return 0;
  }

  // TODO(benvanik): increment thread priority?
  // TODO(benvanik): wait?

  int32_t previous_count = 0;
  [[maybe_unused]] bool success =
      SemaphoreRelease(*sem, static_cast<int32_t>(adjustment), &previous_count);
  return static_cast<uint32_t>(previous_count);
}

u32 KeReleaseSemaphore_entry(ppc_ptr_t<X_KSEMAPHORE> semaphore_ptr, u32 increment, u32 adjustment,
                             u32 wait) {
  return KeReleaseSemaphore(semaphore_ptr.guest_address(), increment, adjustment, wait);
}

u32 NtCreateSemaphore_entry(mapped_u32 handle_ptr, mapped_void obj_attributes_ptr, u32 count,
                            u32 limit) {
  REX_KERNEL_NAMED_CREATE(Semaphore, handle_ptr, obj_attributes_ptr);

  X_HANDLE handle = 0;
  auto added = runtime::Current().object_table().CreateWithBlock<SemaphoreObject>(
      obj_attributes_ptr.guest_address(), sizeof(X_KSEMAPHORE), static_cast<int32_t>(count),
      static_cast<int32_t>(limit));
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XFAILED(status)) {
    return status;
  }
  bool valid = false;
  if (auto sem = runtime::Current().object_table().Get<SemaphoreObject>(handle)) {
    valid = sem->semaphore != nullptr;
  }
  if (!valid) {
    (void)runtime::Current().object_table().Close(handle);
    if (handle_ptr) {
      *handle_ptr = 0;
    }
    return X_STATUS_INVALID_PARAMETER;
  }

  if (handle_ptr) {
    *handle_ptr = handle;
  }

  return X_STATUS_SUCCESS;
}

u32 NtReleaseSemaphore_entry(u32 sem_handle, u32 release_count, mapped_u32 previous_count_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  int32_t previous_count = 0;

  auto sem = runtime::Current().object_table().Get<SemaphoreObject>(sem_handle);
  if (sem) {
    bool success = SemaphoreRelease(*sem, static_cast<int32_t>(release_count), &previous_count);
    if (!success) {
      result = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (previous_count_ptr) {
    *previous_count_ptr = static_cast<uint32_t>(previous_count);
  }

  return result;
}

u32 NtCreateMutant_entry(mapped_u32 handle_out, ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attributes,
                         u32 initial_owner) {
  REX_KERNEL_NAMED_CREATE(Mutant, handle_out, obj_attributes);

  X_HANDLE handle = 0;
  auto added = runtime::Current().object_table().CreateWithBlock<MutantObject>(
      obj_attributes.guest_address(), 0, initial_owner ? true : false);
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XFAILED(status)) {
    return status;
  }
  if (handle_out) {
    *handle_out = handle;
  }

  return X_STATUS_SUCCESS;
}

u32 NtReleaseMutant_entry(u32 mutant_handle, u32 unknown) {
  // This doesn't seem to be supported.
  // int32_t previous_count_ptr = SHIM_GET_ARG_32(2);

  // Whatever arg 1 is all games seem to set it to 0, so whether it's
  // abandon or wait we say false. Which is good, cause they are
  // both ignored.
  REX_ASSERT(unknown == 0);
  bool abandon = false;

  X_STATUS result = X_STATUS_SUCCESS;

  auto mutant = runtime::Current().object_table().Get<MutantObject>(mutant_handle);
  if (mutant) {
    result = MutantRelease(*mutant, abandon);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

void KeInitializeMutant_entry(ppc_ptr_t<X_KMUTANT> mutant, u32 initial_owner) {
  DropStash(mutant->header);
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();
  mutant->header.type = DISPATCHER_MUTANT;
  const bool owned = initial_owner && thread;
  if (initial_owner && !thread) {
    REXLOG_WARN(kernel, "KeInitializeMutant with an initial owner on a thread with no record");
  }
  mutant->header.signal_state = owned ? 0 : 1;
  mutant->owner_thread = owned ? thread->guest_object() : 0;
  mutant->abandoned = 0;
  (void)runtime::Current().object_table().ResolveGuest<MutantObject>(mutant.guest_address(),
                                                                     DISPATCHER_MUTANT);
}

u32 KeReleaseMutant_entry(ppc_ptr_t<X_KMUTANT> mutant, u32 increment, u32 abandoned, u32 wait) {
  auto object =
      runtime::Current().object_table().ResolveGuest<MutantObject>(mutant.guest_address());
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }
  i32 previous = 0;
  const X_STATUS status = MutantRelease(*object, abandoned != 0, &previous);
  if (XFAILED(status)) {
    return status;
  }
  if (abandoned) {
    mutant->header.signal_state = 1;
    mutant->abandoned = 1;
    mutant->owner_thread = 0;
    return static_cast<u32>(previous);
  }
  mutant->header.signal_state = static_cast<u32>(previous + 1);
  if (previous + 1 > 0) {
    mutant->owner_thread = 0;
  }
  return static_cast<u32>(previous);
}

u32 NtCreateTimer_entry(mapped_u32 handle_ptr, mapped_void obj_attributes_ptr, u32 timer_type) {
  // timer_type = NotificationTimer (0) or SynchronizationTimer (1)

  REX_KERNEL_NAMED_CREATE(Timer, handle_ptr, obj_attributes_ptr);

  X_HANDLE handle = 0;
  auto added = runtime::Current().object_table().CreateWithBlock<TimerObject>(
      obj_attributes_ptr.guest_address(), 0, timer_type);
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XFAILED(status)) {
    return status;
  }
  if (handle_ptr) {
    *handle_ptr = handle;
  }

  return X_STATUS_SUCCESS;
}

void KeInitializeTimerEx_entry(ppc_ptr_t<X_KTIMER> timer_ptr, u32 timer_type, u32 proc_type) {
  DropStash(timer_ptr->header);
  timer_ptr->header.type = static_cast<u8>(timer_type + DISPATCHER_MANUAL_RESET_TIMER);
  timer_ptr->header.process_type = static_cast<u8>(proc_type);
  timer_ptr->header.inserted = 0;
  timer_ptr->header.signal_state = 0;
  timer_ptr->due_time = 0;
  timer_ptr->period = 0;
  timer_ptr->dpc = 0u;
  if (!runtime::Current().object_table().ResolveGuest<TimerObject>(timer_ptr.guest_address())) {
    REXLOG_WARN(kernel, "KeInitializeTimerEx({:08X}) could not create the timer",
                timer_ptr.guest_address());
  }
}

u32 NtSetTimerEx_entry(u32 timer_handle, mapped_u64 due_time_ptr,
                       mapped_void routine_ptr /*PTIMERAPCROUTINE*/, u32 unk_one,
                       mapped_void routine_arg, u32 resume, u32 period_ms, u32 unk_zero) {
  REX_ASSERT(unk_one == 1);
  REX_ASSERT(unk_zero == 0);

  uint64_t due_time = *due_time_ptr;

  X_STATUS result = X_STATUS_SUCCESS;

  auto timer = runtime::Current().object_table().Get<TimerObject>(timer_handle);
  if (timer) {
    result =
        TimerSet(*timer, runtime::Current(), static_cast<int64_t>(due_time), period_ms,
                 routine_ptr.guest_address(), routine_arg.guest_address(), resume ? true : false);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtCancelTimer_entry(u32 timer_handle, mapped_u32 current_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto timer = runtime::Current().object_table().Get<TimerObject>(timer_handle);
  if (timer) {
    result = TimerCancel(*timer);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (current_state_ptr) {
    *current_state_ptr = 0;
  }

  return result;
}

namespace {

u32 ArmKeTimer(ppc_ptr_t<X_KTIMER> timer, i64 due_time, u32 period_ms, ppc_ptr_t<XDPC> dpc) {
  auto& runtime = runtime::Current();
  auto object = runtime.object_table().ResolveGuest<TimerObject>(timer.guest_address());
  if (!object) {
    return 0;
  }
  (void)TimerCancel(*object);
  const u32 was_set = timer->header.inserted;
  timer->header.inserted = 1;
  timer->header.signal_state = 0;
  timer->due_time = static_cast<u64>(due_time);
  timer->period = period_ms;
  timer->dpc = dpc.guest_address();
  if (XFAILED(TimerSetDPC(*object, runtime, timer.guest_address(), due_time, period_ms,
                          dpc.guest_address()))) {
    timer->header.inserted = 0;
  }
  return was_set ? 1 : 0;
}

}  // namespace

u32 KeSetTimerEx_entry(ppc_ptr_t<X_KTIMER> timer, u64, u32 period_ms, ppc_ptr_t<XDPC> dpc) {
  return ArmKeTimer(timer, static_cast<i64>(ThreadState::Get()->context()->r4.u64), period_ms, dpc);
}

u32 KeSetTimer_entry(ppc_ptr_t<X_KTIMER> timer, u64, ppc_ptr_t<XDPC> dpc) {
  return ArmKeTimer(timer, static_cast<i64>(ThreadState::Get()->context()->r4.u64), 0, dpc);
}

u32 KeCancelTimer_entry(ppc_ptr_t<X_KTIMER> timer) {
  auto object = runtime::Current().object_table().ResolveGuest<TimerObject>(timer.guest_address());
  if (object) {
    (void)TimerCancel(*object);
  }
  const u32 was_set = timer->header.inserted;
  timer->header.inserted = 0;
  return was_set ? 1 : 0;
}

uint32_t KeWaitForSingleObject(u32 object_address, uint32_t wait_reason, uint32_t processor_mode,
                               uint32_t alertable, uint64_t* timeout_ptr) {
  const X_HANDLE handle =
      runtime::Current().object_table().ResolveGuestObject(object_address, -1).value_or(0);

  if (!handle) {
    // The only kind-of failure code (though this should never happen)
    REX_ASSERT_ALWAYS();
    return X_STATUS_ABANDONED_WAIT_0;
  }

  X_STATUS result =
      WaitOn(runtime::Current(), handle, wait_reason, processor_mode, alertable, timeout_ptr);

  if (alertable && result == X_STATUS_USER_APC) {
    ThreadRecord::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

u32 KeWaitForSingleObject_entry(mapped_void object_ptr, u32 wait_reason, u32 processor_mode,
                                u32 alertable, mapped_u64 timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  // REXLOG_TRACE(kernel, "KeWaitForSingleObject " "obj={:#x} reason={} mode={} alertable={}
  // timeout={}",
  // object_ptr.guest_address(), (uint32_t)wait_reason,
  //(uint32_t)processor_mode, (uint32_t)alertable,
  // timeout_ptr ? (int64_t)timeout : -1);
  auto result = KeWaitForSingleObject(object_ptr.guest_address(), wait_reason, processor_mode,
                                      alertable, timeout_ptr ? &timeout : nullptr);
  // REXLOG_TRACE(kernel, "KeWaitForSingleObject -> " "{:#x}", result);
  return result;
}

u32 NtWaitForSingleObjectEx_entry(u32 object_handle, u32 wait_mode, u32 alertable,
                                  mapped_u64 timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = WaitOn(runtime::Current(), object_handle, 3, wait_mode, alertable,
                           timeout_ptr ? &timeout : nullptr);
  if (alertable && result == X_STATUS_USER_APC) {
    ThreadRecord::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

u32 KeWaitForMultipleObjects_entry(u32 count, mapped_u32 objects_ptr, u32 wait_type,
                                   u32 wait_reason, u32 processor_mode, u32 alertable,
                                   mapped_u64 timeout_ptr, mapped_void wait_block_array_ptr) {
  REX_ASSERT(wait_type <= 1);

  std::vector<X_HANDLE> handles;
  for (uint32_t n = 0; n < count; n++) {
    const X_HANDLE handle = runtime::Current()
                                .object_table()
                                .ResolveGuestObject(static_cast<u32>(objects_ptr[n]), -1)
                                .value_or(0);
    if (!handle) {
      return X_STATUS_INVALID_PARAMETER;
    }

    handles.push_back(handle);
  }

  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = WaitOnMany(runtime::Current(), handles, wait_type, wait_reason, processor_mode,
                               alertable, timeout_ptr ? &timeout : nullptr);
  if (alertable && result == X_STATUS_USER_APC) {
    ThreadRecord::GetCurrentThread()->DeliverAPCs();
  }
  return result;
}

uint32_t NtWaitForMultipleObjectsEx(uint32_t count, be_u32* handles, uint32_t wait_type,
                                    uint32_t wait_mode, uint32_t alertable, uint64_t* timeout_ptr) {
  REX_ASSERT(wait_type <= 1);

  std::vector<X_HANDLE> object_handles(handles, handles + count);

  auto result = WaitOnMany(runtime::Current(), object_handles, wait_type, 6, wait_mode, alertable,
                           timeout_ptr);
  if (alertable && result == X_STATUS_USER_APC) {
    ThreadRecord::GetCurrentThread()->DeliverAPCs();
  }
  return result;
}

u32 NtWaitForMultipleObjectsEx_entry(u32 count, mapped_u32 handles, u32 wait_type, u32 wait_mode,
                                     u32 alertable, mapped_u64 timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  return NtWaitForMultipleObjectsEx(count, handles, wait_type, wait_mode, alertable,
                                    timeout_ptr ? &timeout : nullptr);
}

u32 NtSignalAndWaitForSingleObjectEx_entry(u32 signal_handle, u32 wait_handle, u32 alertable,
                                           u32 r6, mapped_u64 timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = SignalAndWaitOn(runtime::Current(), signal_handle, wait_handle, 3, 1, alertable,
                                    timeout_ptr ? &timeout : nullptr);

  if (alertable && result == X_STATUS_USER_APC) {
    ThreadRecord::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

// Guest memory IRQL helpers - read/write current_irql directly from PCR.
// Take PPCContext* explicitly so they work from any thread (including host threads
// during InitializeGuestObject and dispatch thread creation).
static unsigned char KfRaiseIrql(runtime::Memory& memory, PPCContext* ctx, unsigned char new_irql) {
  auto pcr = memory.TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  uint8_t old_irql = pcr->current_irql;
  pcr->current_irql = new_irql;
  return old_irql;
}

static void KfLowerIrql(runtime::Memory& memory, PPCContext* ctx, unsigned char new_irql) {
  auto pcr = memory.TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  pcr->current_irql = new_irql;
}

// Guest memory spinlock helpers - store PCR address as owner (matching xenia).
// PPCContext* provides r13 (PCR address) without needing ThreadRecord::GetCurrentThread().
uint32_t KeKfAcquireSpinLock(runtime::Memory& memory, PPCContext* ctx, X_KSPINLOCK* lock,
                             bool change_irql) {
  uint32_t old_irql = change_irql ? KfRaiseIrql(memory, ctx, IRQL_DISPATCH) : 0;
  const u32 pcr_addr = static_cast<u32>(ctx->r13.u64);
  REX_ASSERT(lock->prcb_of_owner.load(std::memory_order_relaxed) != pcr_addr);
  for (u32 expected = 0;
       !lock->prcb_of_owner.compare_exchange(expected, pcr_addr, std::memory_order_acquire);
       expected = 0) {
    rex::platform::MaybeYield();
  }
  return old_irql;
}

void KeKfReleaseSpinLock(runtime::Memory& memory, PPCContext* ctx, X_KSPINLOCK* lock,
                         uint32_t old_irql, bool change_irql) {
  REX_ASSERT(lock->prcb_of_owner.load(std::memory_order_relaxed) ==
             static_cast<uint32_t>(ctx->r13.u64));
  lock->prcb_of_owner.store(0, std::memory_order_release);
  if (change_irql && old_irql < IRQL_DISPATCH) {
    KfLowerIrql(memory, ctx, static_cast<unsigned char>(old_irql));
  }
}

// Guest memory APC helpers
void KeInitializeApc(XAPC* apc, uint32_t thread_ptr, uint32_t kernel_routine,
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

uint32_t KeInsertQueueApc(runtime::Memory& memory, XAPC* apc, uint32_t arg1, uint32_t arg2,
                          uint32_t priority_increment, PPCContext* ctx) {
  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto* mem = &memory;
  auto target_thread = mem->TranslateVirtual<X_KTHREAD*>(thread_guest_pointer);
  auto old_irql = KeKfAcquireSpinLock(memory, ctx, &target_thread->apc_lock);
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
  KeKfReleaseSpinLock(memory, ctx, &target_thread->apc_lock, old_irql);
  return result;
}

u32 KfAcquireSpinLock_entry(mapped_u32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  return KeKfAcquireSpinLock(*runtime::Current().memory(), ThreadState::Get()->context(), lock);
}

void KfReleaseSpinLock_entry(mapped_u32 lock_ptr, u32 old_irql) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  KeKfReleaseSpinLock(*runtime::Current().memory(), ThreadState::Get()->context(), lock, old_irql);
  DrainDPCsBelowDispatch(runtime::Current());
}

void KeAcquireSpinLockAtRaisedIrql_entry(mapped_u32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  KeKfAcquireSpinLock(*runtime::Current().memory(), ThreadState::Get()->context(), lock, false);
}

u32 KeTryToAcquireSpinLockAtRaisedIrql_entry(mapped_u32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  auto* ctx = ThreadState::Get()->context();
  u32 expected = 0;
  return lock->prcb_of_owner.compare_exchange(expected, static_cast<u32>(ctx->r13.u64),
                                              std::memory_order_acquire)
             ? 1
             : 0;
}

void KeReleaseSpinLockFromRaisedIrql_entry(mapped_u32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.host_address());
  KeKfReleaseSpinLock(*runtime::Current().memory(), ThreadState::Get()->context(), lock, 0, false);
}

void KeEnterCriticalRegion_entry() {
  ThreadRecord::GetCurrentThread()->EnterCriticalRegion();
}

static_assert(offsetof(PPCContext, f31) - offsetof(PPCContext, f0) == 31 * sizeof(PPCRegister));
static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v0) == 127 * sizeof(PPCVRegister));

// NOLINTNEXTLINE(readability-identifier-naming): frozen export entry name
void KeSaveFloatingPointState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->context();
  DPCSaveArea& area = state->dpc_save_area();
  std::memcpy(area.fpr.data(), &ctx->f0, sizeof(area.fpr));
  area.csr = ctx->fpscr.getcsr();
}

// NOLINTNEXTLINE(readability-identifier-naming): frozen export entry name
void KeRestoreFloatingPointState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->context();
  const DPCSaveArea& area = state->dpc_save_area();
  std::memcpy(&ctx->f0, area.fpr.data(), sizeof(area.fpr));
  ctx->fpscr.csr = area.csr;
  ctx->fpscr.setcsr(area.csr);
}

// NOLINTNEXTLINE(readability-identifier-naming): frozen export entry name
void KeSaveVectorUnitState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->context();
  DPCSaveArea& area = state->dpc_save_area();
  std::memcpy(area.vr.data(), &ctx->v0, sizeof(area.vr));
  area.vscr_sat = ctx->vscr_sat;
}

// NOLINTNEXTLINE(readability-identifier-naming): frozen export entry name
void KeRestoreVectorUnitState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->context();
  const DPCSaveArea& area = state->dpc_save_area();
  std::memcpy(&ctx->v0, area.vr.data(), sizeof(area.vr));
  ctx->vscr_sat = area.vscr_sat;
}

void KeLeaveCriticalRegion_entry() {
  ThreadRecord::GetCurrentThread()->LeaveCriticalRegion();
}

u32 KeRaiseIrqlToDpcLevel_entry() {
  return KfRaiseIrql(*runtime::Current().memory(), ThreadState::Get()->context(), IRQL_DISPATCH);
}

void KfLowerIrql_entry(u32 old_value) {
  auto& runtime = runtime::Current();
  KfLowerIrql(*runtime.memory(), ThreadState::Get()->context(),
              static_cast<unsigned char>(static_cast<uint32_t>(old_value)));
  DrainDPCsBelowDispatch(runtime);
}

u32 KfRaiseIrql_entry(u32 new_irql) {
  return KfRaiseIrql(*runtime::Current().memory(), ThreadState::Get()->context(),
                     static_cast<unsigned char>(static_cast<uint32_t>(new_irql)));
}

void NtQueueApcThread_entry(u32 thread_handle, mapped_void apc_routine,
                            mapped_void apc_routine_context, mapped_void arg1, mapped_void arg2) {
  auto mem = runtime::Current().memory();
  auto thread = runtime::Current().thread_registry().FindByHandle(thread_handle);

  if (!thread) {
    REXLOG_ERROR(kernel, "NtQueueApcThread: Incorrect thread handle! Might cause crash");
    return;
  }

  if (!apc_routine) {
    REXLOG_ERROR(kernel, "NtQueueApcThread: Incorrect apc routine! Might cause crash");
    return;
  }

  uint32_t apc_ptr = mem->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    return;
  }
  XAPC* apc = mem->TranslateVirtual<XAPC*>(apc_ptr);
  KeInitializeApc(apc, thread->guest_object(), XAPC::kDummyKernelRoutine, 0,
                  apc_routine.guest_address(), 1 /* user apc mode */,
                  apc_routine_context.guest_address());

  if (!KeInsertQueueApc(*mem, apc, arg1.guest_address(), arg2.guest_address(), 0,
                        ThreadState::Get()->context())) {
    mem->SystemHeapFree(apc_ptr);
    return;
  }
  // Match Edge/Canary behavior: callback is only a wakeup hint.
  // APC delivery happens via alertable wait handling.
  thread->thread()->QueueUserCallback([]() {});
}

void KeInitializeApc_entry(ppc_ptr_t<XAPC> apc, mapped_void thread_ptr, mapped_void kernel_routine,
                           mapped_void rundown_routine, mapped_void normal_routine,
                           u32 processor_mode, mapped_void normal_context) {
  KeInitializeApc(apc.host_address(), thread_ptr.guest_address(), kernel_routine.guest_address(),
                  rundown_routine.guest_address(), normal_routine.guest_address(), processor_mode,
                  normal_context.guest_address());
}

u32 KeInsertQueueApc_entry(ppc_ptr_t<XAPC> apc, mapped_void arg1, mapped_void arg2,
                           u32 priority_increment) {
  return KeInsertQueueApc(*runtime::Current().memory(), apc.host_address(), arg1.guest_address(),
                          arg2.guest_address(), priority_increment, ThreadState::Get()->context());
}

u32 KeRemoveQueueApc_entry(ppc_ptr_t<XAPC> apc) {
  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto* ctx = ThreadState::Get()->context();
  auto mem = runtime::Current().memory();
  auto target_thread = mem->TranslateVirtual<X_KTHREAD*>(thread_guest_pointer);
  auto old_irql = KeKfAcquireSpinLock(*mem, ctx, &target_thread->apc_lock);

  bool result = false;
  if (apc->enqueued) {
    apc->enqueued = 0;
    util::XeRemoveEntryList(&apc->list_entry, mem);
    result = true;
  }

  KeKfReleaseSpinLock(*mem, ctx, &target_thread->apc_lock, old_irql);
  return result ? 1 : 0;
}

u32 KiApcNormalRoutineNop_entry(u32 unk0 /* output? */, u32 unk1 /* 0x13 */) {
  return 0;
}

void KeInitializeDpc_entry(ppc_ptr_t<XDPC> dpc, mapped_void routine, mapped_void context) {
  dpc->Initialize(routine.guest_address(), context.guest_address());
}

u32 KeInsertQueueDpc_entry(ppc_ptr_t<XDPC> dpc, u32 arg1, u32 arg2) {
  auto& runtime = runtime::Current();
  if (!runtime.dispatcher().QueueDPC(dpc.guest_address(), arg1, arg2)) {
    return 0;
  }
  DrainDPCsBelowDispatch(runtime);
  return 1;
}

u32 KeRemoveQueueDpc_entry(ppc_ptr_t<XDPC> dpc) {
  return runtime::Current().dispatcher().RemoveDPC(dpc.guest_address()) ? 1 : 0;
}

void KeRetireDpcList_entry() {
  runtime::Current().dispatcher().DrainDPCs();
}

struct X_KINTERRUPT {
  be_u32 service_routine;
  be_u32 service_context;
  be_u32 spin_lock;
  be_u32 service_count;
  u8 bus_irql;
  u8 irql;
  u8 mode_connected;
  u8 target_number;
};
REX_STATIC_ASSERT_SIZE(X_KINTERRUPT, 0x14);

constexpr u8 kInterruptConnected = 0x01;

void KeInitializeInterrupt_entry(ppc_ptr_t<X_KINTERRUPT> interrupt, u32 service_routine,
                                 u32 service_context, u32 bus_irql, u32 irql, u32 mode,
                                 u32 target_number) {
  interrupt->service_routine = service_routine;
  interrupt->service_context = service_context;
  interrupt->spin_lock = 0;
  interrupt->service_count = 0;
  interrupt->bus_irql = static_cast<u8>(bus_irql);
  interrupt->irql = static_cast<u8>(irql);
  interrupt->mode_connected = static_cast<u8>((mode & 0x7F) << 1);
  interrupt->target_number = static_cast<u8>(target_number);
}

u32 KeConnectInterrupt_entry(ppc_ptr_t<X_KINTERRUPT> interrupt) {
  std::atomic_ref<u8> state(interrupt->mode_connected);
  return (state.fetch_or(kInterruptConnected) & kInterruptConnected) ? 0 : 1;
}

u32 KeDisconnectInterrupt_entry(ppc_ptr_t<X_KINTERRUPT> interrupt) {
  std::atomic_ref<u8> state(interrupt->mode_connected);
  return (state.fetch_and(static_cast<u8>(~kInterruptConnected)) & kInterruptConnected) ? 1 : 0;
}

u64 KeQueryInterruptTime_entry() {
  const u64 ticks = Clock::QueryGuestTickCount();
  const u64 frequency = Clock::guest_tick_frequency();
  return ticks / frequency * 10000000 + ticks % frequency * 10000000 / frequency;
}

// https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/51e4dfcaacfdbd1a9692272931a436371492f72d/import/OpenXDK/include/xboxkrnl/xboxkrnl.h#L1372
struct X_ERWLOCK {
  be_i32 lock_count;              // 0x0
  be_u32 writers_waiting_count;   // 0x4
  be_u32 readers_waiting_count;   // 0x8
  be_u32 readers_entry_count;     // 0xC
  X_KEVENT writer_event;          // 0x10
  X_KSEMAPHORE reader_semaphore;  // 0x20
  X_KSPINLOCK spin_lock;          // 0x34
};
REX_STATIC_ASSERT_SIZE(X_ERWLOCK, 0x38);

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
  lock_ptr->spin_lock.prcb_of_owner.store(0, std::memory_order_release);
}

void ExAcquireReadWriteLockExclusive_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->context();
  auto old_irql = KeKfAcquireSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count) {
    KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count++;

  KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
  KeWaitForSingleObject(lock_ptr.guest_address() + offsetof(X_ERWLOCK, writer_event), 7, 0, 0,
                        nullptr);
}

u32 ExTryToAcquireReadWriteLockExclusive_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->context();
  auto old_irql = KeKfAcquireSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0) {
    lock_ptr->lock_count = 0;
    result = 1;
  } else {
    result = 0;
  }

  KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
  return result;
}

void ExAcquireReadWriteLockShared_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->context();
  auto old_irql = KeKfAcquireSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count || (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->readers_entry_count++;
    KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->readers_waiting_count++;

  KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
  KeWaitForSingleObject(lock_ptr.guest_address() + offsetof(X_ERWLOCK, reader_semaphore), 7, 0, 0,
                        nullptr);
}

u32 ExTryToAcquireReadWriteLockShared_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->context();
  auto old_irql = KeKfAcquireSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0 ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->lock_count++;
    lock_ptr->readers_entry_count++;
    result = 1;
  } else {
    result = 0;
  }

  KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
  return result;
}

void ExReleaseReadWriteLock_entry(ppc_ptr_t<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->context();
  auto old_irql = KeKfAcquireSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock);

  int32_t lock_count = --lock_ptr->lock_count;

  if (lock_count < 0) {
    lock_ptr->readers_entry_count = 0;
    KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  if (!lock_ptr->readers_entry_count) {
    auto readers_waiting_count = lock_ptr->readers_waiting_count;
    if (readers_waiting_count) {
      lock_ptr->readers_waiting_count = 0;
      lock_ptr->readers_entry_count = readers_waiting_count;
      KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
      KeReleaseSemaphore(lock_ptr.guest_address() + offsetof(X_ERWLOCK, reader_semaphore), 1,
                         readers_waiting_count, 0);
      return;
    }
  }

  auto readers_entry_count = --lock_ptr->readers_entry_count;
  if (readers_entry_count) {
    KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count--;
  KeKfReleaseSpinLock(*runtime::Current().memory(), ctx, &lock_ptr->spin_lock, old_irql);
  KeSetEvent(lock_ptr.guest_address() + offsetof(X_ERWLOCK, writer_event), 1, 0);
}

// NOTE: This function is commonly inlined, and probably won't be called!
u32 InterlockedPushEntrySList_entry(ppc_ptr_t<X_SLIST_HEADER> plist_ptr,
                                    ppc_ptr_t<X_SINGLE_LIST_ENTRY> entry) {
  REX_ASSERT(plist_ptr != nullptr);
  REX_ASSERT(entry != nullptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {};
  uint32_t old_head = 0;
  do {
    old_hdr = *plist_ptr;
    new_hdr.depth = old_hdr.depth + 1;
    new_hdr.sequence = old_hdr.sequence + 1;

    old_head = old_hdr.next.next;
    entry->next = old_hdr.next.next;
    new_hdr.next.next = entry.guest_address();
  } while (!rex::platform::atomic_cas(std::bit_cast<uint64_t>(old_hdr),
                                      std::bit_cast<uint64_t>(new_hdr),
                                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return old_head;
}

u32 InterlockedPopEntrySList_entry(ppc_ptr_t<X_SLIST_HEADER> plist_ptr) {
  REX_ASSERT(plist_ptr != nullptr);

  uint32_t popped = 0;
  alignas(8) X_SLIST_HEADER old_hdr = {};
  alignas(8) X_SLIST_HEADER new_hdr = {};
  do {
    old_hdr = *plist_ptr;
    auto next =
        runtime::Current().memory()->TranslateVirtual<X_SINGLE_LIST_ENTRY*>(old_hdr.next.next);
    if (!old_hdr.next.next) {
      return 0;
    }
    popped = old_hdr.next.next;

    new_hdr.depth = old_hdr.depth - 1;
    new_hdr.next.next = next->next;
    new_hdr.sequence = old_hdr.sequence;
  } while (!rex::platform::atomic_cas(std::bit_cast<uint64_t>(old_hdr),
                                      std::bit_cast<uint64_t>(new_hdr),
                                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return popped;
}

u32 InterlockedFlushSList_entry(ppc_ptr_t<X_SLIST_HEADER> plist_ptr) {
  REX_ASSERT(plist_ptr != nullptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {};
  uint32_t first = 0;
  do {
    old_hdr = *plist_ptr;
    first = old_hdr.next.next;
    new_hdr.next.next = 0;
    new_hdr.depth = 0;
    new_hdr.sequence = 0;
  } while (!rex::platform::atomic_cas(std::bit_cast<uint64_t>(old_hdr),
                                      std::bit_cast<uint64_t>(new_hdr),
                                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return first;
}

REX_EXPORT_STUB(__imp__KeInsertByKeyDeviceQueue);
REX_EXPORT_STUB(__imp__KeInsertDeviceQueue);
REX_EXPORT_STUB(__imp__KeInsertHeadQueue);
REX_EXPORT_STUB(__imp__KeInsertQueue);
REX_EXPORT_STUB(__imp__KeRemoveByKeyDeviceQueue);
REX_EXPORT_STUB(__imp__KeRemoveDeviceQueue);
REX_EXPORT_STUB(__imp__KeRemoveEntryDeviceQueue);
REX_EXPORT_STUB(__imp__KeRemoveQueue);
REX_EXPORT_STUB(__imp__KeSetEventBoostPriority);
REX_EXPORT_STUB(__imp__KiApcNormalRoutineNop_);

u32 KeSuspendThread_entry(mapped_void kthread_ptr) {
  auto thread = FindThread(kthread_ptr.guest_address());
  uint32_t old_suspend_count = 0;

  if (thread) {
    old_suspend_count = thread->suspend_count();
    uint32_t discarded = 0;
    thread->Suspend(&discarded);
  }

  return old_suspend_count;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__ExCreateThread, rex::kernel::xboxkrnl::ExCreateThread_entry)
REX_EXPORT(__imp__ExTerminateThread, rex::kernel::xboxkrnl::ExTerminateThread_entry)
REX_EXPORT(__imp__NtResumeThread, rex::kernel::xboxkrnl::NtResumeThread_entry)
REX_EXPORT(__imp__KeResumeThread, rex::kernel::xboxkrnl::KeResumeThread_entry)
REX_EXPORT(__imp__NtSuspendThread, rex::kernel::xboxkrnl::NtSuspendThread_entry)
REX_EXPORT(__imp__KeSuspendThread, rex::kernel::xboxkrnl::KeSuspendThread_entry)
REX_EXPORT(__imp__KeSetCurrentStackPointers, rex::kernel::xboxkrnl::KeSetCurrentStackPointers_entry)
REX_EXPORT(__imp__KeSetAffinityThread, rex::kernel::xboxkrnl::KeSetAffinityThread_entry)
REX_EXPORT(__imp__KeQueryBasePriorityThread, rex::kernel::xboxkrnl::KeQueryBasePriorityThread_entry)
REX_EXPORT(__imp__KeSetBasePriorityThread, rex::kernel::xboxkrnl::KeSetBasePriorityThread_entry)
REX_EXPORT(__imp__KeSetDisableBoostThread, rex::kernel::xboxkrnl::KeSetDisableBoostThread_entry)
REX_EXPORT(__imp__KeGetCurrentProcessType, rex::kernel::xboxkrnl::KeGetCurrentProcessType_entry)
REX_EXPORT(__imp__KeSetCurrentProcessType, rex::kernel::xboxkrnl::KeSetCurrentProcessType_entry)
REX_EXPORT(__imp__KeQueryPerformanceFrequency,
           rex::kernel::xboxkrnl::KeQueryPerformanceFrequency_entry)
REX_EXPORT(__imp__KeDelayExecutionThread, rex::kernel::xboxkrnl::KeDelayExecutionThread_entry)
REX_EXPORT(__imp__NtYieldExecution, rex::kernel::xboxkrnl::NtYieldExecution_entry)
REX_EXPORT(__imp__KeQuerySystemTime, rex::kernel::xboxkrnl::KeQuerySystemTime_entry)
REX_EXPORT(__imp__KeTlsAlloc, rex::kernel::xboxkrnl::KeTlsAlloc_entry)
REX_EXPORT(__imp__KeTlsFree, rex::kernel::xboxkrnl::KeTlsFree_entry)
REX_EXPORT(__imp__KeTlsGetValue, rex::kernel::xboxkrnl::KeTlsGetValue_entry)
REX_EXPORT(__imp__KeTlsSetValue, rex::kernel::xboxkrnl::KeTlsSetValue_entry)
REX_EXPORT(__imp__KeInitializeEvent, rex::kernel::xboxkrnl::KeInitializeEvent_entry)
REX_EXPORT(__imp__KeSetEvent, rex::kernel::xboxkrnl::KeSetEvent_entry)
REX_EXPORT(__imp__KePulseEvent, rex::kernel::xboxkrnl::KePulseEvent_entry)
REX_EXPORT(__imp__KeResetEvent, rex::kernel::xboxkrnl::KeResetEvent_entry)
REX_EXPORT(__imp__NtCreateEvent, rex::kernel::xboxkrnl::NtCreateEvent_entry)
REX_EXPORT(__imp__NtSetEvent, rex::kernel::xboxkrnl::NtSetEvent_entry)
REX_EXPORT(__imp__NtPulseEvent, rex::kernel::xboxkrnl::NtPulseEvent_entry)
REX_EXPORT(__imp__NtClearEvent, rex::kernel::xboxkrnl::NtClearEvent_entry)
REX_EXPORT(__imp__KeInitializeSemaphore, rex::kernel::xboxkrnl::KeInitializeSemaphore_entry)
REX_EXPORT(__imp__KeReleaseSemaphore, rex::kernel::xboxkrnl::KeReleaseSemaphore_entry)
REX_EXPORT(__imp__NtCreateSemaphore, rex::kernel::xboxkrnl::NtCreateSemaphore_entry)
REX_EXPORT(__imp__NtReleaseSemaphore, rex::kernel::xboxkrnl::NtReleaseSemaphore_entry)
REX_EXPORT(__imp__NtCreateMutant, rex::kernel::xboxkrnl::NtCreateMutant_entry)
REX_EXPORT(__imp__NtReleaseMutant, rex::kernel::xboxkrnl::NtReleaseMutant_entry)
REX_EXPORT(__imp__KeInitializeMutant, rex::kernel::xboxkrnl::KeInitializeMutant_entry)
REX_EXPORT(__imp__KeReleaseMutant, rex::kernel::xboxkrnl::KeReleaseMutant_entry)
REX_EXPORT(__imp__NtCreateTimer, rex::kernel::xboxkrnl::NtCreateTimer_entry)
REX_EXPORT(__imp__NtSetTimerEx, rex::kernel::xboxkrnl::NtSetTimerEx_entry)
REX_EXPORT(__imp__NtCancelTimer, rex::kernel::xboxkrnl::NtCancelTimer_entry)
REX_EXPORT(__imp__KeInitializeTimerEx, rex::kernel::xboxkrnl::KeInitializeTimerEx_entry)
REX_EXPORT(__imp__KeSetTimerEx, rex::kernel::xboxkrnl::KeSetTimerEx_entry)
REX_EXPORT(__imp__KeSetTimer, rex::kernel::xboxkrnl::KeSetTimer_entry)
REX_EXPORT(__imp__KeCancelTimer, rex::kernel::xboxkrnl::KeCancelTimer_entry)
REX_EXPORT(__imp__KeInitializeInterrupt, rex::kernel::xboxkrnl::KeInitializeInterrupt_entry)
REX_EXPORT(__imp__KeConnectInterrupt, rex::kernel::xboxkrnl::KeConnectInterrupt_entry)
REX_EXPORT(__imp__KeDisconnectInterrupt, rex::kernel::xboxkrnl::KeDisconnectInterrupt_entry)
REX_EXPORT(__imp__KeQueryInterruptTime, rex::kernel::xboxkrnl::KeQueryInterruptTime_entry)
REX_EXPORT(__imp__KeWaitForSingleObject, rex::kernel::xboxkrnl::KeWaitForSingleObject_entry)
REX_EXPORT(__imp__NtWaitForSingleObjectEx, rex::kernel::xboxkrnl::NtWaitForSingleObjectEx_entry)
REX_EXPORT(__imp__KeWaitForMultipleObjects, rex::kernel::xboxkrnl::KeWaitForMultipleObjects_entry)
REX_EXPORT(__imp__NtWaitForMultipleObjectsEx,
           rex::kernel::xboxkrnl::NtWaitForMultipleObjectsEx_entry)
REX_EXPORT(__imp__NtSignalAndWaitForSingleObjectEx,
           rex::kernel::xboxkrnl::NtSignalAndWaitForSingleObjectEx_entry)
REX_EXPORT(__imp__KfAcquireSpinLock, rex::kernel::xboxkrnl::KfAcquireSpinLock_entry)
REX_EXPORT(__imp__KfReleaseSpinLock, rex::kernel::xboxkrnl::KfReleaseSpinLock_entry)
REX_EXPORT(__imp__KeAcquireSpinLockAtRaisedIrql,
           rex::kernel::xboxkrnl::KeAcquireSpinLockAtRaisedIrql_entry)
REX_EXPORT(__imp__KeTryToAcquireSpinLockAtRaisedIrql,
           rex::kernel::xboxkrnl::KeTryToAcquireSpinLockAtRaisedIrql_entry)
REX_EXPORT(__imp__KeReleaseSpinLockFromRaisedIrql,
           rex::kernel::xboxkrnl::KeReleaseSpinLockFromRaisedIrql_entry)
REX_EXPORT(__imp__KeEnterCriticalRegion, rex::kernel::xboxkrnl::KeEnterCriticalRegion_entry)
REX_EXPORT(__imp__KeLeaveCriticalRegion, rex::kernel::xboxkrnl::KeLeaveCriticalRegion_entry)
REX_EXPORT(__imp__KeRaiseIrqlToDpcLevel, rex::kernel::xboxkrnl::KeRaiseIrqlToDpcLevel_entry)
REX_EXPORT(__imp__KfLowerIrql, rex::kernel::xboxkrnl::KfLowerIrql_entry)
REX_EXPORT(__imp__NtQueueApcThread, rex::kernel::xboxkrnl::NtQueueApcThread_entry)
REX_EXPORT(__imp__KeInitializeApc, rex::kernel::xboxkrnl::KeInitializeApc_entry)
REX_EXPORT(__imp__KeInsertQueueApc, rex::kernel::xboxkrnl::KeInsertQueueApc_entry)
REX_EXPORT(__imp__KeRemoveQueueApc, rex::kernel::xboxkrnl::KeRemoveQueueApc_entry)
REX_EXPORT(__imp__KiApcNormalRoutineNop, rex::kernel::xboxkrnl::KiApcNormalRoutineNop_entry)
REX_EXPORT(__imp__KeInitializeDpc, rex::kernel::xboxkrnl::KeInitializeDpc_entry)
REX_EXPORT(__imp__KeInsertQueueDpc, rex::kernel::xboxkrnl::KeInsertQueueDpc_entry)
REX_EXPORT(__imp__KeRemoveQueueDpc, rex::kernel::xboxkrnl::KeRemoveQueueDpc_entry)
REX_EXPORT(__imp__KeRetireDpcList, rex::kernel::xboxkrnl::KeRetireDpcList_entry)
REX_EXPORT(__imp__ExInitializeReadWriteLock, rex::kernel::xboxkrnl::ExInitializeReadWriteLock_entry)
REX_EXPORT(__imp__ExAcquireReadWriteLockExclusive,
           rex::kernel::xboxkrnl::ExAcquireReadWriteLockExclusive_entry)
REX_EXPORT(__imp__ExTryToAcquireReadWriteLockExclusive,
           rex::kernel::xboxkrnl::ExTryToAcquireReadWriteLockExclusive_entry)
REX_EXPORT(__imp__ExAcquireReadWriteLockShared,
           rex::kernel::xboxkrnl::ExAcquireReadWriteLockShared_entry)
REX_EXPORT(__imp__ExTryToAcquireReadWriteLockShared,
           rex::kernel::xboxkrnl::ExTryToAcquireReadWriteLockShared_entry)
REX_EXPORT(__imp__ExReleaseReadWriteLock, rex::kernel::xboxkrnl::ExReleaseReadWriteLock_entry)
REX_EXPORT(__imp__InterlockedPushEntrySList, rex::kernel::xboxkrnl::InterlockedPushEntrySList_entry)
REX_EXPORT(__imp__InterlockedPopEntrySList, rex::kernel::xboxkrnl::InterlockedPopEntrySList_entry)
REX_EXPORT(__imp__InterlockedFlushSList, rex::kernel::xboxkrnl::InterlockedFlushSList_entry)

REX_EXPORT_STUB(__imp__KeAlertResumeThread);
REX_EXPORT_STUB(__imp__KeAlertThread);
REX_EXPORT_STUB(__imp__KeBoostPriorityThread);
REX_EXPORT_STUB(__imp__KeContextFromKframes);
REX_EXPORT_STUB(__imp__KeContextToKframes);
REX_EXPORT_STUB(__imp__KeFlushCacheRange);
REX_EXPORT_STUB(__imp__KeFlushCurrentEntireTb);
REX_EXPORT_STUB(__imp__KeFlushEntireTb);
REX_EXPORT_STUB(__imp__KeFlushMultipleTb);
REX_EXPORT_STUB(__imp__KeFlushUserModeCurrentTb);
REX_EXPORT_STUB(__imp__KeFlushUserModeTb);
REX_EXPORT_STUB(__imp__KeInitializeDeviceQueue);
REX_EXPORT_STUB(__imp__KeInitializeQueue);
REX_EXPORT_STUB(__imp__KeIpiGenericCall);
REX_EXPORT_STUB(__imp__KeQueryBackgroundProcessors);
REX_EXPORT_STUB(__imp__KeRegisterDriverNotification);
REX_EXPORT(__imp__KeRestoreFloatingPointState,
           rex::kernel::xboxkrnl::KeRestoreFloatingPointState_entry)
REX_EXPORT(__imp__KeRestoreVectorUnitState, rex::kernel::xboxkrnl::KeRestoreVectorUnitState_entry)
REX_EXPORT_STUB(__imp__KeRundownQueue);
REX_EXPORT(__imp__KeSaveFloatingPointState, rex::kernel::xboxkrnl::KeSaveFloatingPointState_entry)
REX_EXPORT(__imp__KeSaveVectorUnitState, rex::kernel::xboxkrnl::KeSaveVectorUnitState_entry)
REX_EXPORT_STUB(__imp__KeSetBackgroundProcessors);
REX_EXPORT_STUB(__imp__KeSetPriorityClassThread);
REX_EXPORT_STUB(__imp__KeSetPriorityThread);
REX_EXPORT_STUB(__imp__KeStallExecutionProcessor);
REX_EXPORT_STUB(__imp__KeSweepDcacheRange);
REX_EXPORT_STUB(__imp__KeSweepIcacheRange);
REX_EXPORT_STUB(__imp__KeTestAlertThread);
REX_EXPORT(__imp__KfRaiseIrql, rex::kernel::xboxkrnl::KfRaiseIrql_entry)
