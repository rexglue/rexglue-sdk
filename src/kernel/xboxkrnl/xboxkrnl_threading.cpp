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
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/runtime/dispatcher.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_registry.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/kernel_string.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/mutant_object.h>
#include <rex/runtime/semaphore_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/timer_object.h>
#include <rex/runtime/xtypes.h>
#include "kernel/argument_check.h"
#include "kernel/object_lookup.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

namespace {

ThreadPin FindThread(u32 thread_address) {
  const X_HANDLE handle =
      runtime::GetCurrent().GetObjectTable().ResolveHandle(thread_address, -1).value_or(0);
  return handle ? runtime::GetCurrent().GetThreadRegistry().FindByHandle(handle) : ThreadPin{};
}

void DrainDPCsBelowDispatch(Runtime& runtime) {
  auto* ctx = ThreadState::Get()->GetContext();
  auto* pcr = runtime.GetMemory()->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  if (pcr->current_irql < kIRQLDispatch) {
    runtime.GetDispatcher().DrainDPCs();
  }
}

void DropStash(X_DISPATCH_HEADER& header) {
  if (header.wait_list_flink == kStashSignature) {
    runtime::CloseOwned(runtime::GetCurrent().GetObjectTable(), header.wait_list_blink);
  }
  header.wait_list_flink = 0;
  header.wait_list_blink = 0;
}

}  // namespace

// r13+0x100 is the thread state, ID at 0x14C, last error at 0x160 stored only while 0x150 is 0.

u32 ExCreateThread_entry(MappedU32 handle_ptr, u32 stack_size, MappedU32 thread_id_ptr,
                         u32 xapi_thread_startup, MappedVoid start_address,
                         MappedVoid start_context, u32 creation_flags) {
  REXLOG_TRACE(kernel,
               "ExCreateThread "
               "stack={:#x} xapi_startup={:#x} start={:#x} context={:#x} flags={:#x}",
               static_cast<u32>(stack_size), static_cast<u32>(xapi_thread_startup),
               start_address.GetGuestAddress(), start_context.GetGuestAddress(),
               static_cast<u32>(creation_flags));
  auto& runtime = runtime::GetCurrent();
  u32 title_process = runtime.GetTitleProcess();
  if (creation_flags & 2) {
    REXLOG_WARN(kernel, "ExCreateThread Title is creating a system thread!");
    title_process = runtime.GetSystemProcess();
  }

  auto thread = std::make_unique<KernelThread>(
      runtime, stack_size, xapi_thread_startup, start_address.GetGuestAddress(),
      start_context.GetGuestAddress(), creation_flags, true, false, title_process);
  KernelThread* raw = thread.get();

  X_HANDLE handle = 0;
  auto launched = runtime.GetThreadRegistry().Launch(std::move(thread));
  if (launched) {
    handle = *launched;
  }
  X_STATUS result = ToXStatus(launched);
  if (XFAILED(result)) {
    REX_FAIL(kernel, result, "Thread creation failed: {:08X}", result);
  }

  const u32 thread_id = raw->GetThreadId();
  if (handle_ptr) {
    if (creation_flags & 0x80) {
      auto created_thread = runtime.GetThreadRegistry().FindByHandle(handle);
      *handle_ptr = created_thread ? created_thread->GetGuestObject() : 0;
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
               result, handle_ptr ? static_cast<u32>(*handle_ptr) : 0,
               thread_id_ptr ? static_cast<u32>(*thread_id_ptr) : 0);
  return result;
}

u32 ExTerminateThread_entry(u32 exit_code) {
  KernelThread* thread = KernelThread::GetCurrentThread();

  thread->Exit(static_cast<int>(exit_code));
}

u32 NtResumeThread_entry(u32 handle, MappedU32 suspend_count_ptr) {
  X_RESULT result = X_STATUS_INVALID_HANDLE;
  u32 suspend_count = 0;

  auto thread = runtime::GetCurrent().GetThreadRegistry().FindByHandle(handle);
  if (thread) {
    REXLOG_TRACE(kernel, "NtResumeThread handle={:08X} thread={}", static_cast<u32>(handle),
                 thread->GetName());
    result = thread->Resume(&suspend_count);
  } else {
    REXLOG_WARN(kernel, "NtResumeThread handle={:08X} NOT FOUND", static_cast<u32>(handle));
  }
  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  REXLOG_TRACE(kernel, "NtResumeThread -> {:#x} suspend_count={}", result, suspend_count);
  return result;
}

u32 KeResumeThread_entry(MappedVoid thread_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  auto thread = FindThread(thread_ptr.GetGuestAddress());
  if (thread) {
    REXLOG_TRACE(kernel, "KeResumeThread ptr={:08X} thread={}", thread_ptr.GetGuestAddress(),
                 thread->GetName());
    result = thread->Resume();
  } else {
    REXLOG_WARN(kernel, "KeResumeThread ptr={:08X} NOT FOUND", thread_ptr.GetGuestAddress());
    result = X_STATUS_INVALID_HANDLE;
  }

  REXLOG_TRACE(kernel, "KeResumeThread -> {:#x}", result);
  return result;
}

u32 NtSuspendThread_entry(u32 handle, MappedU32 suspend_count_ptr) {
  X_RESULT result = X_STATUS_SUCCESS;
  u32 suspend_count = 0;

  auto thread = runtime::GetCurrent().GetThreadRegistry().FindByHandle(handle);
  if (thread) {
    auto guest_thread = thread->GetGuestObject<X_KTHREAD>();
    if (guest_thread->terminated) {
      return X_STATUS_THREAD_IS_TERMINATING;
    }

    REXLOG_TRACE(kernel, "NtSuspendThread handle={:08X} thread={}", static_cast<u32>(handle),
                 thread->GetName());
    result = thread->Suspend(&suspend_count);
  } else {
    REXLOG_WARN(kernel, "NtSuspendThread handle={:08X} NOT FOUND", static_cast<u32>(handle));
    result = X_STATUS_INVALID_HANDLE;
  }

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  REXLOG_TRACE(kernel, "NtSuspendThread -> {:#x} suspend_count={}", result, suspend_count);
  return result;
}

namespace {

void UpdateGuestStackPointers(X_KTHREAD* kthread, X_KPCR* pcr, PPCContext* ctx, u32 sp,
                              u32 stack_alloc_base, u32 stack_base, u32 stack_limit) {
  kthread->stack_alloc_base = stack_alloc_base;
  kthread->stack_base = stack_base;
  kthread->stack_limit = stack_limit;
  pcr->stack_base_ptr = stack_base;
  pcr->stack_end_ptr = stack_limit;
  ctx->r1.u64 = sp;
}

}  // namespace

void KeSetCurrentStackPointers_entry(MappedVoid stack_ptr, GuestPtrT<X_KTHREAD> thread,
                                     MappedVoid stack_alloc_base, MappedVoid stack_base,
                                     MappedVoid stack_limit) {
  auto current_thread = KernelThread::GetCurrentThread();
  auto context = current_thread->GetThreadState()->GetContext();
  auto pcr = runtime::GetCurrent().GetMemory()->TranslateVirtual<X_KPCR*>(
      static_cast<u32>(context->r13.u64));

  UpdateGuestStackPointers(thread, pcr, context, stack_ptr.GetGuestAddress(),
                           stack_alloc_base.GetValue(), stack_base.GetValue(),
                           stack_limit.GetValue());
}

/**
 * Sets the affinity of a thread. Unlike Windows NT, the previous affinity is stored through the
 * pointer argument and the return value is the status.
 *
 * @param thread_ptr The thread object.
 * @param affinity The new affinity mask.
 * @param previous_affinity_ptr Receives the previous affinity, may be null.
 * @return X_STATUS_SUCCESS or X_STATUS_INVALID_HANDLE.
 */
u32 KeSetAffinityThread_entry(MappedVoid thread_ptr, u32 affinity,
                              MappedU32 previous_affinity_ptr) {
  REXLOG_TRACE(kernel,
               "KeSetAffinityThread "
               "thread={:#x} affinity={:#x}",
               thread_ptr.GetGuestAddress(), static_cast<u32>(affinity));
  REX_KERNEL_ARG(affinity);
  auto thread = FindThread(thread_ptr.GetGuestAddress());
  if (!thread) {
    REXLOG_DEV(kernel,
               "KeSetAffinityThread "
               "invalid thread handle ptr={:#x}",
               thread_ptr.GetGuestAddress());
    return X_STATUS_INVALID_HANDLE;
  }
  if (previous_affinity_ptr) {
    *previous_affinity_ptr = static_cast<u32>(1) << thread->GetActiveCPU();
  }
  thread->SetAffinity(affinity);
  REXLOG_TRACE(kernel,
               "KeSetAffinityThread -> "
               "0x0");
  return X_STATUS_SUCCESS;
}

u32 KeQueryBasePriorityThread_entry(MappedVoid thread_ptr) {
  REXLOG_TRACE(kernel,
               "KeQueryBasePriorityThread "
               "thread={:#x}",
               thread_ptr.GetGuestAddress());
  i32 priority = 0;

  auto thread = FindThread(thread_ptr.GetGuestAddress());
  if (thread) {
    priority = thread->QueryPriority();
  }

  REXLOG_TRACE(kernel,
               "KeQueryBasePriorityThread -> "
               "{}",
               priority);
  return priority;
}

u32 KeSetBasePriorityThread_entry(MappedVoid thread_ptr, u32 increment) {
  REXLOG_TRACE(kernel,
               "KeSetBasePriorityThread "
               "thread={:#x} increment={}",
               thread_ptr.GetGuestAddress(), static_cast<i32>(increment));
  i32 prev_priority = 0;
  auto thread = FindThread(thread_ptr.GetGuestAddress());

  if (thread) {
    prev_priority = thread->QueryPriority();
    thread->SetPriority(static_cast<i32>(increment));
  }
  REXLOG_TRACE(kernel,
               "KeSetBasePriorityThread -> "
               "prev_priority={}",
               prev_priority);

  return prev_priority;
}

u32 KeSetDisableBoostThread_entry(GuestPtrT<X_KTHREAD> thread_ptr, u32 disabled) {
  auto old_boost_disabled = reinterpret_cast<std::atomic_uint8_t*>(&thread_ptr->boost_disabled)
                                ->exchange(static_cast<u8>(disabled));
  return old_boost_disabled;
}

u32 KeGetCurrentProcessType_entry() {
  auto current_thread = KernelThread::GetCurrentThread();
  auto context = current_thread->GetThreadState()->GetContext();
  auto pcr = runtime::GetCurrent().GetMemory()->TranslateVirtual<X_KPCR*>(
      static_cast<u32>(context->r13.u64));

  if (pcr->prcb_data.dpc_active) {
    return pcr->processtype_value_in_dpc;
  }

  auto thread = runtime::GetCurrent().GetMemory()->TranslateVirtual<X_KTHREAD*>(
      pcr->prcb_data.current_thread);
  return thread->process_type;
}

void KeSetCurrentProcessType_entry(u32 type) {
  REX_ASSERT(type <= 2);

  runtime::GetCurrent().SetProcessType(type);

  auto current_thread = KernelThread::GetCurrentThread();
  auto context = current_thread->GetThreadState()->GetContext();
  auto pcr = runtime::GetCurrent().GetMemory()->TranslateVirtual<X_KPCR*>(
      static_cast<u32>(context->r13.u64));

  if (pcr->prcb_data.dpc_active) {
    pcr->processtype_value_in_dpc = static_cast<u8>(type);
  }
}

u32 KeQueryPerformanceFrequency_entry() {
  u64 result = Clock::GetGuestTickFrequency();
  return static_cast<u32>(result);
}

u32 KeDelayExecutionThread_entry(u32 processor_mode, u32 alertable, MappedU64 interval_ptr) {
  KernelThread* thread = KernelThread::GetCurrentThread();

  if (alertable && thread->HasPendingAPCs()) {
    thread->DeliverAPCs();
    return X_STATUS_USER_APC;
  }

  X_STATUS result = thread->Delay(processor_mode, alertable, *interval_ptr);

  if (alertable && result == X_STATUS_USER_APC) {
    thread->DeliverAPCs();
  }

  return result;
}

u32 NtYieldExecution_entry() {
  KernelThread::CheckStop();
  KernelThread::WriteKernelTime();
  rex::platform::MaybeYield();
  return X_STATUS_SUCCESS;
}

void KeQuerySystemTime_entry(MappedU64 time_ptr) {
  u64 time = Clock::QueryGuestSystemTime();
  if (time_ptr) {
    *time_ptr = time;
  }
}

u32 KeTlsAlloc_entry() {
  u32 slot = runtime::GetCurrent()
                 .GetThreadRegistry()
                 .AllocateTLS(ThreadState::Get()->GetContext())
                 .value_or(kXTLSOutOfIndexes);
  if (slot != kXTLSOutOfIndexes) {
    KernelThread::GetCurrentThread()->SetTLSValue(slot, 0);
  }
  REXLOG_TRACE(kernel,
               "KeTlsAlloc -> "
               "slot={}",
               slot);
  return slot;
}

u32 KeTlsFree_entry(u32 tls_index) {
  REXLOG_TRACE(kernel,
               "KeTlsFree "
               "slot={}",
               static_cast<u32>(tls_index));
  if (tls_index == kXTLSOutOfIndexes) {
    REXLOG_TRACE(kernel,
                 "KeTlsFree -> "
                 "0 (invalid)");
    return 0;
  }

  runtime::GetCurrent().GetThreadRegistry().FreeTLS(ThreadState::Get()->GetContext(), tls_index);
  REXLOG_TRACE(kernel,
               "KeTlsFree -> "
               "1");
  return 1;
}

/**
 * Returns the value of a TLS slot. Always succeeds, even when the slot overflows the TLS.
 *
 * @param tls_index The TLS slot.
 * @return The slot value, or 0.
 */
u32 KeTlsGetValue_entry(u32 tls_index) {
  u32 value = 0;
  if (KernelThread::GetCurrentThread()->GetTLSValue(tls_index, &value)) {
    return value;
  }

  return 0;
}

/**
 * Sets the value of a TLS slot.
 *
 * An index past the slots is undefined behavior in the XDK. Here it stores nothing and returns 0.
 *
 * @param tls_index The TLS slot.
 * @param tls_value The new value.
 * @return 1 when the value was stored, otherwise 0.
 */
u32 KeTlsSetValue_entry(u32 tls_index, u32 tls_value) {
  REXLOG_TRACE(kernel,
               "KeTlsSetValue "
               "slot={} value={:#x}",
               static_cast<u32>(tls_index), static_cast<u32>(tls_value));
  if (KernelThread::GetCurrentThread()->SetTLSValue(tls_index, tls_value)) {
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

void KeInitializeEvent_entry(GuestPtrT<X_KEVENT> event_ptr, u32 event_type, u32 initial_state) {
  auto& table = runtime::GetCurrent().GetObjectTable();
  if (event_ptr->header.wait_list_flink == kStashSignature) {
    if (auto existing = table.Get<EventObject>(event_ptr->header.wait_list_blink);
        existing && existing->manual_reset == (event_type == 0)) {
      event_ptr->header.type = static_cast<u8>(event_type);
      event_ptr->header.signal_state = initial_state;
      initial_state ? existing->Set() : existing->Reset();
      return;
    }
    DropStash(event_ptr->header);
  }
  event_ptr.Zero();
  event_ptr->header.type = static_cast<u8>(event_type);
  event_ptr->header.signal_state = initial_state;
  if (!table.Resolve<EventObject>(event_ptr.GetGuestAddress(), static_cast<i32>(event_type))) {
    REXLOG_WARN(kernel, "KeInitializeEvent({:08X}) could not create the event",
                event_ptr.GetGuestAddress());
  }
}

u32 KeSetEvent(u32 event_address, [[maybe_unused]] u32 increment, [[maybe_unused]] u32 wait) {
  auto ev = runtime::GetCurrent().GetObjectTable().Resolve<EventObject>(event_address);
  if (!ev) {
    REXLOG_WARN(kernel, "KeSetEvent({:08X}) found no event", event_address);
    return 0;
  }

  return ev->Set();
}

u32 KeSetEvent_entry(GuestPtrT<X_KEVENT> event_ptr, u32 increment, u32 wait) {
  return KeSetEvent(event_ptr.GetGuestAddress(), increment, wait);
}

u32 KePulseEvent_entry(GuestPtrT<X_KEVENT> event_ptr, [[maybe_unused]] u32 increment,
                       [[maybe_unused]] u32 wait) {
  auto ev =
      runtime::GetCurrent().GetObjectTable().Resolve<EventObject>(event_ptr.GetGuestAddress());
  if (!ev) {
    REXLOG_WARN(kernel, "KePulseEvent({:08X}) found no event", event_ptr.GetGuestAddress());
    return 0;
  }

  return ev->Pulse();
}

u32 KeResetEvent_entry(GuestPtrT<X_KEVENT> event_ptr) {
  auto ev =
      runtime::GetCurrent().GetObjectTable().Resolve<EventObject>(event_ptr.GetGuestAddress());
  if (!ev) {
    REXLOG_WARN(kernel, "KeResetEvent({:08X}) found no event", event_ptr.GetGuestAddress());
    return 0;
  }

  return ev->Reset();
}

u32 NtCreateEvent_entry(MappedU32 handle_ptr, GuestPtrT<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
                        u32 event_type, u32 initial_state) {
  REX_KERNEL_NAMED_CREATE(kEvent, handle_ptr, obj_attributes_ptr);

  X_HANDLE handle = 0;
  auto added = runtime::GetCurrent().GetObjectTable().CreateObject<EventObject>(
      obj_attributes_ptr.GetGuestAddress(), sizeof(X_KEVENT), !event_type, !!initial_state);
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

u32 NtSetEvent(u32 handle, be_u32* previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::GetCurrent().GetObjectTable().Get<EventObject>(handle);
  if (ev) {
    i32 was_signalled = ev->Set();
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<u32>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtSetEvent_entry(u32 handle, MappedU32 previous_state_ptr) {
  return NtSetEvent(handle, previous_state_ptr);
}

u32 NtPulseEvent_entry(u32 handle, MappedU32 previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::GetCurrent().GetObjectTable().Get<EventObject>(handle);
  if (ev) {
    i32 was_signalled = ev->Pulse();
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<u32>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtClearEvent(u32 handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::GetCurrent().GetObjectTable().Get<EventObject>(handle);
  if (ev) {
    ev->Reset();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtClearEvent_entry(u32 handle) {
  return NtClearEvent(handle);
}

void KeInitializeSemaphore_entry(GuestPtrT<X_KSEMAPHORE> semaphore_ptr, u32 count, u32 limit) {
  // Type 5 is SemaphoreObject.
  semaphore_ptr->header.type = 5;
  semaphore_ptr->header.signal_state = count;
  semaphore_ptr->limit = limit;

  auto sem = runtime::GetCurrent().GetObjectTable().Resolve<SemaphoreObject>(
      semaphore_ptr.GetGuestAddress(), 5);
  if (!sem) {
    REXLOG_WARN(kernel, "KeInitializeSemaphore({:08X}) could not create the semaphore",
                semaphore_ptr.GetGuestAddress());
  }
}

u32 KeReleaseSemaphore(u32 semaphore_address, [[maybe_unused]] u32 increment, u32 adjustment,
                       [[maybe_unused]] u32 wait) {
  auto sem = runtime::GetCurrent().GetObjectTable().Resolve<SemaphoreObject>(semaphore_address);
  if (!sem) {
    REXLOG_WARN(kernel, "KeReleaseSemaphore({:08X}) found no semaphore", semaphore_address);
    return 0;
  }

  i32 previous_count = 0;
  [[maybe_unused]] bool success = sem->Release(static_cast<i32>(adjustment), &previous_count);
  return static_cast<u32>(previous_count);
}

u32 KeReleaseSemaphore_entry(GuestPtrT<X_KSEMAPHORE> semaphore_ptr, u32 increment, u32 adjustment,
                             u32 wait) {
  return KeReleaseSemaphore(semaphore_ptr.GetGuestAddress(), increment, adjustment, wait);
}

u32 NtCreateSemaphore_entry(MappedU32 handle_ptr, MappedVoid obj_attributes_ptr, u32 count,
                            u32 limit) {
  REX_KERNEL_NAMED_CREATE(kSemaphore, handle_ptr, obj_attributes_ptr);

  X_HANDLE handle = 0;
  auto added = runtime::GetCurrent().GetObjectTable().CreateObject<SemaphoreObject>(
      obj_attributes_ptr.GetGuestAddress(), sizeof(X_KSEMAPHORE), static_cast<i32>(count),
      static_cast<i32>(limit));
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XFAILED(status)) {
    return status;
  }
  bool valid = false;
  if (auto sem = runtime::GetCurrent().GetObjectTable().Get<SemaphoreObject>(handle)) {
    valid = sem->semaphore != nullptr;
  }
  if (!valid) {
    runtime::CloseOwned(runtime::GetCurrent().GetObjectTable(), handle);
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

u32 NtReleaseSemaphore_entry(u32 sem_handle, u32 release_count, MappedU32 previous_count_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  i32 previous_count = 0;

  auto sem = runtime::GetCurrent().GetObjectTable().Get<SemaphoreObject>(sem_handle);
  if (sem) {
    bool success = sem->Release(static_cast<i32>(release_count), &previous_count);
    if (!success) {
      result = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (previous_count_ptr) {
    *previous_count_ptr = static_cast<u32>(previous_count);
  }

  return result;
}

u32 NtCreateMutant_entry(MappedU32 handle_out, GuestPtrT<X_OBJECT_ATTRIBUTES> obj_attributes,
                         u32 initial_owner) {
  REX_KERNEL_NAMED_CREATE(kMutant, handle_out, obj_attributes);

  X_HANDLE handle = 0;
  auto added = runtime::GetCurrent().GetObjectTable().CreateObject<MutantObject>(
      obj_attributes.GetGuestAddress(), 0, initial_owner ? true : false);
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

/**
 * Releases a mutant.
 *
 * @param mutant_handle The mutant handle.
 * @param unknown The abandon or wait flag, always 0 and ignored.
 * @return X_STATUS_SUCCESS or an error status.
 */
u32 NtReleaseMutant_entry(u32 mutant_handle, u32 unknown) {
  REX_ASSERT(unknown == 0);
  bool abandon = false;

  X_STATUS result = X_STATUS_SUCCESS;

  auto mutant = runtime::GetCurrent().GetObjectTable().Get<MutantObject>(mutant_handle);
  if (mutant) {
    result = mutant->Release(abandon);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

void KeInitializeMutant_entry(GuestPtrT<X_KMUTANT> mutant, u32 initial_owner) {
  DropStash(mutant->header);
  KernelThread* thread = KernelThread::GetCurrentThread();
  mutant->header.type = kDispatcherMutant;
  const bool owned = initial_owner && thread;
  if (initial_owner && !thread) {
    REXLOG_WARN(kernel, "KeInitializeMutant with an initial owner and no thread");
  }
  mutant->header.signal_state = owned ? 0 : 1;
  mutant->owner_thread = owned ? thread->GetGuestObject() : 0;
  mutant->abandoned = 0;
  (void)runtime::GetCurrent().GetObjectTable().Resolve<MutantObject>(mutant.GetGuestAddress(),
                                                                     kDispatcherMutant);
}

u32 KeReleaseMutant_entry(GuestPtrT<X_KMUTANT> mutant, [[maybe_unused]] u32 increment,
                          u32 abandoned, [[maybe_unused]] u32 wait) {
  auto object =
      runtime::GetCurrent().GetObjectTable().Resolve<MutantObject>(mutant.GetGuestAddress());
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }
  i32 previous = 0;
  const X_STATUS status = object->Release(abandoned != 0, &previous);
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

/**
 * Creates a timer.
 *
 * @param handle_ptr Receives the timer handle.
 * @param obj_attributes_ptr The optional object attributes.
 * @param timer_type 0 for NotificationTimer or 1 for SynchronizationTimer.
 * @return X_STATUS_SUCCESS or an error status.
 */
u32 NtCreateTimer_entry(MappedU32 handle_ptr, MappedVoid obj_attributes_ptr, u32 timer_type) {
  REX_KERNEL_NAMED_CREATE(kTimer, handle_ptr, obj_attributes_ptr);

  X_HANDLE handle = 0;
  auto added = runtime::GetCurrent().GetObjectTable().CreateObject<TimerObject>(
      obj_attributes_ptr.GetGuestAddress(), 0, timer_type);
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

void KeInitializeTimerEx_entry(GuestPtrT<X_KTIMER> timer_ptr, u32 timer_type, u32 proc_type) {
  DropStash(timer_ptr->header);
  timer_ptr->header.type = static_cast<u8>(timer_type + kDispatcherManualResetTimer);
  timer_ptr->header.process_type = static_cast<u8>(proc_type);
  timer_ptr->header.inserted = 0;
  timer_ptr->header.signal_state = 0;
  timer_ptr->due_time = 0;
  timer_ptr->period = 0;
  timer_ptr->dpc = 0u;
  if (!runtime::GetCurrent().GetObjectTable().Resolve<TimerObject>(timer_ptr.GetGuestAddress())) {
    REXLOG_WARN(kernel, "KeInitializeTimerEx({:08X}) could not create the timer",
                timer_ptr.GetGuestAddress());
  }
}

u32 NtSetTimerEx_entry(u32 timer_handle, MappedU64 due_time_ptr, MappedVoid routine_ptr,
                       u32 unk_one, MappedVoid routine_arg, u32 resume, u32 period_ms,
                       u32 unk_zero) {
  REX_ASSERT(unk_one == 1);
  REX_ASSERT(unk_zero == 0);

  u64 due_time = *due_time_ptr;

  X_STATUS result = X_STATUS_SUCCESS;

  auto timer = runtime::GetCurrent().GetObjectTable().Get<TimerObject>(timer_handle);
  if (timer) {
    result = timer->Set(runtime::GetCurrent(), static_cast<i64>(due_time), period_ms,
                        routine_ptr.GetGuestAddress(), routine_arg.GetGuestAddress(),
                        resume ? true : false);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 NtCancelTimer_entry(u32 timer_handle, MappedU32 current_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto timer = runtime::GetCurrent().GetObjectTable().Get<TimerObject>(timer_handle);
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

namespace {

u32 ArmKeTimer(GuestPtrT<X_KTIMER> timer, i64 due_time, u32 period_ms, GuestPtrT<X_KDPC> dpc) {
  auto& runtime = runtime::GetCurrent();
  auto object = runtime.GetObjectTable().Resolve<TimerObject>(timer.GetGuestAddress());
  if (!object) {
    return 0;
  }
  (void)object->Cancel();
  const u32 was_set = timer->header.inserted;
  timer->header.inserted = 1;
  timer->header.signal_state = 0;
  timer->due_time = static_cast<u64>(due_time);
  timer->period = period_ms;
  timer->dpc = dpc.GetGuestAddress();
  if (XFAILED(object->SetDPC(runtime, timer.GetGuestAddress(), due_time, period_ms,
                             dpc.GetGuestAddress()))) {
    timer->header.inserted = 0;
  }
  return was_set ? 1 : 0;
}

}  // namespace

u32 KeSetTimerEx_entry(GuestPtrT<X_KTIMER> timer, i64 due_time, u32 period_ms,
                       GuestPtrT<X_KDPC> dpc) {
  return ArmKeTimer(timer, due_time, period_ms, dpc);
}

u32 KeSetTimer_entry(GuestPtrT<X_KTIMER> timer, i64 due_time, GuestPtrT<X_KDPC> dpc) {
  return ArmKeTimer(timer, due_time, 0, dpc);
}

u32 KeCancelTimer_entry(GuestPtrT<X_KTIMER> timer) {
  auto object =
      runtime::GetCurrent().GetObjectTable().Resolve<TimerObject>(timer.GetGuestAddress());
  if (object) {
    (void)object->Cancel();
  }
  const u32 was_set = timer->header.inserted;
  timer->header.inserted = 0;
  return was_set ? 1 : 0;
}

u32 KeWaitForSingleObject(u32 object_address, u32 wait_reason, u32 processor_mode, u32 alertable,
                          u64* timeout_ptr) {
  const X_HANDLE handle =
      runtime::GetCurrent().GetObjectTable().ResolveHandle(object_address, -1).value_or(0);

  if (!handle) {
    REXLOG_WARN(kernel, "KeWaitForSingleObject({:08X}) found no object", object_address);
    return X_STATUS_ABANDONED_WAIT_0;
  }

  X_STATUS result =
      Wait(runtime::GetCurrent(), handle, wait_reason, processor_mode, alertable, timeout_ptr);

  if (alertable && result == X_STATUS_USER_APC) {
    KernelThread::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

u32 KeWaitForSingleObject_entry(MappedVoid object_ptr, u32 wait_reason, u32 processor_mode,
                                u32 alertable, MappedU64 timeout_ptr) {
  u64 timeout = timeout_ptr ? static_cast<u64>(*timeout_ptr) : 0u;
  auto result = KeWaitForSingleObject(object_ptr.GetGuestAddress(), wait_reason, processor_mode,
                                      alertable, timeout_ptr ? &timeout : nullptr);
  return result;
}

u32 NtWaitForSingleObjectEx_entry(u32 object_handle, u32 wait_mode, u32 alertable,
                                  MappedU64 timeout_ptr) {
  u64 timeout = timeout_ptr ? static_cast<u64>(*timeout_ptr) : 0u;
  X_STATUS result = Wait(runtime::GetCurrent(), object_handle, 3, wait_mode, alertable,
                         timeout_ptr ? &timeout : nullptr);
  if (alertable && result == X_STATUS_USER_APC) {
    KernelThread::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

u32 KeWaitForMultipleObjects_entry(u32 count, MappedU32 objects_ptr, u32 wait_type, u32 wait_reason,
                                   u32 processor_mode, u32 alertable, MappedU64 timeout_ptr,
                                   [[maybe_unused]] MappedVoid wait_block_array_ptr) {
  REX_ASSERT(wait_type <= 1);

  std::vector<X_HANDLE> handles;
  for (u32 n = 0; n < count; n++) {
    const X_HANDLE handle = runtime::GetCurrent()
                                .GetObjectTable()
                                .ResolveHandle(static_cast<u32>(objects_ptr[n]), -1)
                                .value_or(0);
    if (!handle) {
      return X_STATUS_INVALID_PARAMETER;
    }

    handles.push_back(handle);
  }

  u64 timeout = timeout_ptr ? static_cast<u64>(*timeout_ptr) : 0u;
  X_STATUS result = WaitMultiple(runtime::GetCurrent(), handles, wait_type, wait_reason,
                                 processor_mode, alertable, timeout_ptr ? &timeout : nullptr);
  if (alertable && result == X_STATUS_USER_APC) {
    KernelThread::GetCurrentThread()->DeliverAPCs();
  }
  return result;
}

u32 NtWaitForMultipleObjectsEx(u32 count, be_u32* handles, u32 wait_type, u32 wait_mode,
                               u32 alertable, u64* timeout_ptr) {
  REX_ASSERT(wait_type <= 1);

  std::vector<X_HANDLE> object_handles(handles, handles + count);

  auto result = WaitMultiple(runtime::GetCurrent(), object_handles, wait_type, 6, wait_mode,
                             alertable, timeout_ptr);
  if (alertable && result == X_STATUS_USER_APC) {
    KernelThread::GetCurrentThread()->DeliverAPCs();
  }
  return result;
}

u32 NtWaitForMultipleObjectsEx_entry(u32 count, MappedU32 handles, u32 wait_type, u32 wait_mode,
                                     u32 alertable, MappedU64 timeout_ptr) {
  u64 timeout = timeout_ptr ? static_cast<u64>(*timeout_ptr) : 0u;
  return NtWaitForMultipleObjectsEx(count, handles, wait_type, wait_mode, alertable,
                                    timeout_ptr ? &timeout : nullptr);
}

u32 NtSignalAndWaitForSingleObjectEx_entry(u32 signal_handle, u32 wait_handle, u32 alertable,
                                           [[maybe_unused]] u32 r6, MappedU64 timeout_ptr) {
  u64 timeout = timeout_ptr ? static_cast<u64>(*timeout_ptr) : 0u;
  X_STATUS result = SignalAndWait(runtime::GetCurrent(), signal_handle, wait_handle, 3, 1,
                                  alertable, timeout_ptr ? &timeout : nullptr);

  if (alertable && result == X_STATUS_USER_APC) {
    KernelThread::GetCurrentThread()->DeliverAPCs();
  }

  return result;
}

/**
 * Raises the IRQL stored in the PCR. Takes the context as an argument so it works from any
 * thread, including host threads.
 *
 * @param memory The guest memory.
 * @param ctx The context whose r13 addresses the PCR.
 * @param new_irql The new IRQL.
 * @return The previous IRQL.
 */
static unsigned char KfRaiseIrql(runtime::Memory& memory, PPCContext* ctx, unsigned char new_irql) {
  auto pcr = memory.TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  u8 old_irql = pcr->current_irql;
  pcr->current_irql = new_irql;
  return old_irql;
}

/**
 * Lowers the IRQL stored in the PCR.
 *
 * @param memory The guest memory.
 * @param ctx The context whose r13 addresses the PCR.
 * @param new_irql The new IRQL.
 */
static void KfLowerIrql(runtime::Memory& memory, PPCContext* ctx, unsigned char new_irql) {
  auto pcr = memory.TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  pcr->current_irql = new_irql;
}

u32 KeKfAcquireSpinLock(runtime::Memory& memory, PPCContext* ctx, X_KSPINLOCK* lock,
                        bool change_irql) {
  u32 old_irql = change_irql ? KfRaiseIrql(memory, ctx, kIRQLDispatch) : 0;
  const u32 pcr_addr = static_cast<u32>(ctx->r13.u64);
  REX_ASSERT(lock->prcb_of_owner.Load(std::memory_order_relaxed) != pcr_addr);
  for (u32 expected = 0;
       !lock->prcb_of_owner.CompareExchange(expected, pcr_addr, std::memory_order_acquire);
       expected = 0) {
    rex::platform::MaybeYield();
  }
  return old_irql;
}

void KeKfReleaseSpinLock(runtime::Memory& memory, PPCContext* ctx, X_KSPINLOCK* lock, u32 old_irql,
                         bool change_irql) {
  REX_ASSERT(lock->prcb_of_owner.Load(std::memory_order_relaxed) == static_cast<u32>(ctx->r13.u64));
  lock->prcb_of_owner.Store(0, std::memory_order_release);
  if (change_irql && old_irql < kIRQLDispatch) {
    KfLowerIrql(memory, ctx, static_cast<unsigned char>(old_irql));
  }
}

void KeInitializeApc(XAPC* apc, u32 thread_ptr, u32 kernel_routine, u32 rundown_routine,
                     u32 normal_routine, u32 apc_mode, u32 normal_context) {
  apc->thread_ptr = thread_ptr;
  apc->kernel_routine = kernel_routine;
  apc->rundown_routine = rundown_routine;
  apc->normal_routine = normal_routine;
  // Type 18 is ApcObject.
  apc->type = 18;
  if (normal_routine) {
    apc->apc_mode = apc_mode;
    apc->normal_context = normal_context;
  } else {
    apc->apc_mode = 0;
    apc->normal_context = 0;
  }
  apc->enqueued = 0;
}

u32 KeInsertQueueApc(runtime::Memory& memory, XAPC* apc, u32 arg1, u32 arg2,
                     [[maybe_unused]] u32 priority_increment, PPCContext* ctx) {
  u32 thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto* mem = &memory;
  auto target_thread = mem->TranslateVirtual<X_KTHREAD*>(thread_guest_pointer);
  auto old_irql = KeKfAcquireSpinLock(memory, ctx, &target_thread->apc_lock);
  u32 result;
  if (!target_thread->may_queue_apcs || apc->enqueued) {
    result = 0;
  } else {
    apc->arg1 = arg1;
    apc->arg2 = arg2;

    auto& which_list = target_thread->apc_lists[apc->apc_mode];

    if (apc->normal_routine) {
      which_list.InsertTail(apc, mem);
    } else {
      // Kernel mode APCs without a normal_routine go before those with one.
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
        runtime::InsertHeadList(insertion_pos->list_entry.blink_ptr, &apc->list_entry, mem);
      }
    }

    apc->enqueued = 1;
    result = 1;
  }
  KeKfReleaseSpinLock(memory, ctx, &target_thread->apc_lock, old_irql);
  return result;
}

u32 KfAcquireSpinLock_entry(MappedU32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.GetHostAddress());
  return KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ThreadState::Get()->GetContext(),
                             lock);
}

void KfReleaseSpinLock_entry(MappedU32 lock_ptr, u32 old_irql) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.GetHostAddress());
  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ThreadState::Get()->GetContext(), lock,
                      old_irql);
  DrainDPCsBelowDispatch(runtime::GetCurrent());
}

void KeAcquireSpinLockAtRaisedIrql_entry(MappedU32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.GetHostAddress());
  KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ThreadState::Get()->GetContext(), lock,
                      false);
}

u32 KeTryToAcquireSpinLockAtRaisedIrql_entry(MappedU32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.GetHostAddress());
  auto* ctx = ThreadState::Get()->GetContext();
  u32 expected = 0;
  return lock->prcb_of_owner.CompareExchange(expected, static_cast<u32>(ctx->r13.u64),
                                             std::memory_order_acquire)
             ? 1
             : 0;
}

void KeReleaseSpinLockFromRaisedIrql_entry(MappedU32 lock_ptr) {
  auto lock = reinterpret_cast<X_KSPINLOCK*>(lock_ptr.GetHostAddress());
  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ThreadState::Get()->GetContext(), lock, 0,
                      false);
}

void KeEnterCriticalRegion_entry() {
  KernelThread::GetCurrentThread()->EnterCriticalRegion();
}

static_assert(offsetof(PPCContext, f31) - offsetof(PPCContext, f0) == 31 * sizeof(PPCRegister));
static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v0) == 127 * sizeof(PPCVRegister));

void KeSaveFloatingPointState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->GetContext();
  DPCSaveArea& area = state->GetDPCSaveArea();
  std::memcpy(area.fpr.data(), &ctx->f0, sizeof(area.fpr));
  area.csr = ctx->fpscr.GetCSR();
}

void KeRestoreFloatingPointState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->GetContext();
  const DPCSaveArea& area = state->GetDPCSaveArea();
  std::memcpy(&ctx->f0, area.fpr.data(), sizeof(area.fpr));
  ctx->fpscr.csr = area.csr;
  ctx->fpscr.SetCSR(area.csr);
}

void KeSaveVectorUnitState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->GetContext();
  DPCSaveArea& area = state->GetDPCSaveArea();
  std::memcpy(area.vr.data(), &ctx->v0, sizeof(area.vr));
  area.vscr_sat = ctx->vscr_sat;
  area.vscr_nj = ctx->vscr_nj;
}

void KeRestoreVectorUnitState_entry() {
  ThreadState* state = ThreadState::Get();
  PPCContext* ctx = state->GetContext();
  const DPCSaveArea& area = state->GetDPCSaveArea();
  std::memcpy(&ctx->v0, area.vr.data(), sizeof(area.vr));
  ctx->vscr_sat = area.vscr_sat;
  ctx->vscr_nj = area.vscr_nj;
}

void KeLeaveCriticalRegion_entry() {
  KernelThread::GetCurrentThread()->LeaveCriticalRegion();
}

u32 KeRaiseIrqlToDpcLevel_entry() {
  return KfRaiseIrql(*runtime::GetCurrent().GetMemory(), ThreadState::Get()->GetContext(),
                     kIRQLDispatch);
}

void KfLowerIrql_entry(u32 old_value) {
  auto& runtime = runtime::GetCurrent();
  KfLowerIrql(*runtime.GetMemory(), ThreadState::Get()->GetContext(), static_cast<u8>(old_value));
  DrainDPCsBelowDispatch(runtime);
}

u32 KfRaiseIrql_entry(u32 new_irql) {
  return KfRaiseIrql(*runtime::GetCurrent().GetMemory(), ThreadState::Get()->GetContext(),
                     static_cast<u8>(new_irql));
}

void NtQueueApcThread_entry(u32 thread_handle, MappedVoid apc_routine,
                            MappedVoid apc_routine_context, MappedVoid arg1, MappedVoid arg2) {
  auto mem = runtime::GetCurrent().GetMemory();
  auto thread = runtime::GetCurrent().GetThreadRegistry().FindByHandle(thread_handle);

  if (!thread) {
    REXLOG_ERROR(kernel, "NtQueueApcThread: Incorrect thread handle! Might cause crash");
    return;
  }

  if (!apc_routine) {
    REXLOG_ERROR(kernel, "NtQueueApcThread: Incorrect apc routine! Might cause crash");
    return;
  }

  u32 apc_ptr = mem->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    return;
  }
  XAPC* apc = mem->TranslateVirtual<XAPC*>(apc_ptr);
  // apc_mode 1 is user mode.
  KeInitializeApc(apc, thread->GetGuestObject(), XAPC::kDummyKernelRoutine, 0,
                  apc_routine.GetGuestAddress(), 1, apc_routine_context.GetGuestAddress());

  if (!KeInsertQueueApc(*mem, apc, arg1.GetGuestAddress(), arg2.GetGuestAddress(), 0,
                        ThreadState::Get()->GetContext())) {
    mem->SystemHeapFree(apc_ptr);
    return;
  }
  // The callback is only a wakeup hint, APCs are delivered by alertable wait handling.
  thread->GetThread()->QueueUserCallback([]() {});
}

void KeInitializeApc_entry(GuestPtrT<XAPC> apc, MappedVoid thread_ptr, MappedVoid kernel_routine,
                           MappedVoid rundown_routine, MappedVoid normal_routine,
                           u32 processor_mode, MappedVoid normal_context) {
  KeInitializeApc(apc.GetHostAddress(), thread_ptr.GetGuestAddress(),
                  kernel_routine.GetGuestAddress(), rundown_routine.GetGuestAddress(),
                  normal_routine.GetGuestAddress(), processor_mode,
                  normal_context.GetGuestAddress());
}

u32 KeInsertQueueApc_entry(GuestPtrT<XAPC> apc, MappedVoid arg1, MappedVoid arg2,
                           u32 priority_increment) {
  return KeInsertQueueApc(*runtime::GetCurrent().GetMemory(), apc.GetHostAddress(),
                          arg1.GetGuestAddress(), arg2.GetGuestAddress(), priority_increment,
                          ThreadState::Get()->GetContext());
}

u32 KeRemoveQueueApc_entry(GuestPtrT<XAPC> apc) {
  u32 thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto* ctx = ThreadState::Get()->GetContext();
  auto mem = runtime::GetCurrent().GetMemory();
  auto target_thread = mem->TranslateVirtual<X_KTHREAD*>(thread_guest_pointer);
  auto old_irql = KeKfAcquireSpinLock(*mem, ctx, &target_thread->apc_lock);

  bool result = false;
  if (apc->enqueued) {
    apc->enqueued = 0;
    runtime::RemoveEntryList(&apc->list_entry, mem);
    result = true;
  }

  KeKfReleaseSpinLock(*mem, ctx, &target_thread->apc_lock, old_irql);
  return result ? 1 : 0;
}

u32 KiApcNormalRoutineNop_entry([[maybe_unused]] u32 unk0, [[maybe_unused]] u32 unk1) {
  return 0;
}

void KeInitializeDpc_entry(GuestPtrT<X_KDPC> dpc, MappedVoid routine, MappedVoid context) {
  dpc->Initialize(routine.GetGuestAddress(), context.GetGuestAddress());
}

u32 KeInsertQueueDpc_entry(GuestPtrT<X_KDPC> dpc, u32 arg1, u32 arg2) {
  auto& runtime = runtime::GetCurrent();
  if (!runtime.GetDispatcher().QueueDPC(dpc.GetGuestAddress(), arg1, arg2)) {
    return 0;
  }
  DrainDPCsBelowDispatch(runtime);
  return 1;
}

u32 KeRemoveQueueDpc_entry(GuestPtrT<X_KDPC> dpc) {
  return runtime::GetCurrent().GetDispatcher().RemoveDPC(dpc.GetGuestAddress()) ? 1 : 0;
}

void KeRetireDpcList_entry() {
  runtime::GetCurrent().GetDispatcher().DrainDPCs();
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

void KeInitializeInterrupt_entry(GuestPtrT<X_KINTERRUPT> interrupt, u32 service_routine,
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

u32 KeConnectInterrupt_entry(GuestPtrT<X_KINTERRUPT> interrupt) {
  std::atomic_ref<u8> state(interrupt->mode_connected);
  return (state.fetch_or(kInterruptConnected) & kInterruptConnected) ? 0 : 1;
}

u32 KeDisconnectInterrupt_entry(GuestPtrT<X_KINTERRUPT> interrupt) {
  std::atomic_ref<u8> state(interrupt->mode_connected);
  return (state.fetch_and(static_cast<u8>(~kInterruptConnected)) & kInterruptConnected) ? 1 : 0;
}

u64 KeQueryInterruptTime_entry() {
  const u64 ticks = Clock::QueryGuestTickCount();
  const u64 frequency = Clock::GetGuestTickFrequency();
  return ticks / frequency * 10000000 + ticks % frequency * 10000000 / frequency;
}

struct X_ERWLOCK {
  be_i32 lock_count;
  be_u32 writers_waiting_count;
  be_u32 readers_waiting_count;
  be_u32 readers_entry_count;
  X_KEVENT writer_event;
  X_KSEMAPHORE reader_semaphore;
  X_KSPINLOCK spin_lock;
};
REX_STATIC_ASSERT_SIZE(X_ERWLOCK, 0x38);

void ExInitializeReadWriteLock_entry(GuestPtrT<X_ERWLOCK> lock_ptr) {
  lock_ptr->lock_count = -1;
  lock_ptr->writers_waiting_count = 0;
  lock_ptr->readers_waiting_count = 0;
  lock_ptr->readers_entry_count = 0;
  GuestPtrT<X_KEVENT> event_ptr(&lock_ptr->writer_event,
                                lock_ptr.GetGuestAddress() + offsetof(X_ERWLOCK, writer_event));
  GuestPtrT<X_KSEMAPHORE> sem_ptr(
      &lock_ptr->reader_semaphore,
      lock_ptr.GetGuestAddress() + offsetof(X_ERWLOCK, reader_semaphore));
  KeInitializeEvent_entry(event_ptr, 1, 0);
  KeInitializeSemaphore_entry(sem_ptr, 0, 0x7FFFFFFF);
  lock_ptr->spin_lock.prcb_of_owner.Store(0, std::memory_order_release);
}

void ExAcquireReadWriteLockExclusive_entry(GuestPtrT<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->GetContext();
  auto old_irql =
      KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock);

  i32 lock_count = ++lock_ptr->lock_count;
  if (!lock_count) {
    KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count++;

  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
  KeWaitForSingleObject(lock_ptr.GetGuestAddress() + offsetof(X_ERWLOCK, writer_event), 7, 0, 0,
                        nullptr);
}

u32 ExTryToAcquireReadWriteLockExclusive_entry(GuestPtrT<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->GetContext();
  auto old_irql =
      KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock);

  u32 result;
  if (lock_ptr->lock_count < 0) {
    lock_ptr->lock_count = 0;
    result = 1;
  } else {
    result = 0;
  }

  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
  return result;
}

void ExAcquireReadWriteLockShared_entry(GuestPtrT<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->GetContext();
  auto old_irql =
      KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock);

  i32 lock_count = ++lock_ptr->lock_count;
  if (!lock_count || (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->readers_entry_count++;
    KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->readers_waiting_count++;

  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
  KeWaitForSingleObject(lock_ptr.GetGuestAddress() + offsetof(X_ERWLOCK, reader_semaphore), 7, 0, 0,
                        nullptr);
}

u32 ExTryToAcquireReadWriteLockShared_entry(GuestPtrT<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->GetContext();
  auto old_irql =
      KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock);

  u32 result;
  if (lock_ptr->lock_count < 0 ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->lock_count++;
    lock_ptr->readers_entry_count++;
    result = 1;
  } else {
    result = 0;
  }

  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
  return result;
}

void ExReleaseReadWriteLock_entry(GuestPtrT<X_ERWLOCK> lock_ptr) {
  auto* ctx = ThreadState::Get()->GetContext();
  auto old_irql =
      KeKfAcquireSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock);

  i32 lock_count = --lock_ptr->lock_count;

  if (lock_count < 0) {
    lock_ptr->readers_entry_count = 0;
    KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  if (!lock_ptr->readers_entry_count) {
    auto readers_waiting_count = lock_ptr->readers_waiting_count;
    if (readers_waiting_count) {
      lock_ptr->readers_waiting_count = 0;
      lock_ptr->readers_entry_count = readers_waiting_count;
      KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
      KeReleaseSemaphore(lock_ptr.GetGuestAddress() + offsetof(X_ERWLOCK, reader_semaphore), 1,
                         readers_waiting_count, 0);
      return;
    }
  }

  auto readers_entry_count = --lock_ptr->readers_entry_count;
  if (readers_entry_count) {
    KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count--;
  KeKfReleaseSpinLock(*runtime::GetCurrent().GetMemory(), ctx, &lock_ptr->spin_lock, old_irql);
  KeSetEvent(lock_ptr.GetGuestAddress() + offsetof(X_ERWLOCK, writer_event), 1, 0);
}

namespace {

bool SListCompareExchange(GuestPtrT<X_SLIST_HEADER> plist_ptr, const X_SLIST_HEADER& expected,
                          const X_SLIST_HEADER& desired) {
  std::atomic_ref<u64> header(*reinterpret_cast<u64*>(plist_ptr.GetHostAddress()));
  u64 compare = std::bit_cast<u64>(expected);
  return header.compare_exchange_strong(compare, std::bit_cast<u64>(desired));
}

}  // namespace

/**
 * Pushes an entry onto an interlocked singly linked list. Titles commonly inline this function.
 *
 * @param plist_ptr The list header.
 * @param entry The entry to push.
 * @return The previous first entry.
 */
u32 InterlockedPushEntrySList_entry(GuestPtrT<X_SLIST_HEADER> plist_ptr,
                                    GuestPtrT<X_SINGLE_LIST_ENTRY> entry) {
  REX_ASSERT(plist_ptr != nullptr);
  REX_ASSERT(entry != nullptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {};
  u32 old_head = 0;
  do {
    old_hdr = *plist_ptr;
    new_hdr.depth = old_hdr.depth + 1;
    new_hdr.sequence = old_hdr.sequence + 1;

    old_head = old_hdr.next.next;
    entry->next = old_hdr.next.next;
    new_hdr.next.next = entry.GetGuestAddress();
  } while (!SListCompareExchange(plist_ptr, old_hdr, new_hdr));

  return old_head;
}

u32 InterlockedPopEntrySList_entry(GuestPtrT<X_SLIST_HEADER> plist_ptr) {
  REX_ASSERT(plist_ptr != nullptr);

  u32 popped = 0;
  alignas(8) X_SLIST_HEADER old_hdr = {};
  alignas(8) X_SLIST_HEADER new_hdr = {};
  do {
    old_hdr = *plist_ptr;
    auto next = runtime::GetCurrent().GetMemory()->TranslateVirtual<X_SINGLE_LIST_ENTRY*>(
        old_hdr.next.next);
    if (!old_hdr.next.next) {
      return 0;
    }
    popped = old_hdr.next.next;

    new_hdr.depth = old_hdr.depth - 1;
    new_hdr.next.next = next->next;
    new_hdr.sequence = old_hdr.sequence;
  } while (!SListCompareExchange(plist_ptr, old_hdr, new_hdr));

  return popped;
}

u32 InterlockedFlushSList_entry(GuestPtrT<X_SLIST_HEADER> plist_ptr) {
  REX_ASSERT(plist_ptr != nullptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {};
  u32 first = 0;
  do {
    old_hdr = *plist_ptr;
    first = old_hdr.next.next;
    new_hdr.next.next = 0;
    new_hdr.depth = 0;
    new_hdr.sequence = 0;
  } while (!SListCompareExchange(plist_ptr, old_hdr, new_hdr));

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

u32 KeSuspendThread_entry(MappedVoid kthread_ptr) {
  auto thread = FindThread(kthread_ptr.GetGuestAddress());
  u32 old_suspend_count = 0;

  if (thread) {
    old_suspend_count = thread->SuspendCount();
    u32 discarded = 0;
    thread->Suspend(&discarded);
  }

  return old_suspend_count;
}

void KeEnableFpuExceptions_entry([[maybe_unused]] u32 enabled) {}

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
REX_EXPORT(__imp__KeEnableFpuExceptions, rex::kernel::xboxkrnl::KeEnableFpuExceptions_entry)
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
