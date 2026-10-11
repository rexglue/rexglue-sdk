/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {

/**
 * Signals an event.
 *
 * @param handle The event handle.
 * @param previous_state_ptr Receives the previous signal state, or null to skip.
 * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE when the handle is not an event.
 */
REX_RUNTIME_API u32 NtSetEvent(u32 handle, be_u32* previous_state_ptr);
/**
 * Sets an event to the nonsignaled state.
 *
 * @param handle The event handle.
 * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE when the handle is not an event.
 */
REX_RUNTIME_API u32 NtClearEvent(u32 handle);

/**
 * Waits on several handles at once.
 *
 * @param count Number of handles.
 * @param handles The handles to wait on.
 * @param wait_type 0 to wait for all handles, 1 to wait for any handle.
 * @param wait_mode The processor mode of the wait.
 * @param alertable Nonzero when a user APC can interrupt the wait.
 * @param timeout_ptr The timeout in 100 ns units, or null to wait indefinitely.
 * @return The wait status, such as the index of the signaled handle or X_STATUS_TIMEOUT.
 */
REX_RUNTIME_API u32 NtWaitForMultipleObjectsEx(u32 count, be_u32* handles, u32 wait_type,
                                               u32 wait_mode, u32 alertable, u64* timeout_ptr);

/**
 * Waits on a kernel object in guest memory.
 *
 * @param object_address Guest address of the kernel object.
 * @param wait_reason The reason for the wait.
 * @param processor_mode The processor mode of the wait.
 * @param alertable Nonzero when a user APC can interrupt the wait.
 * @param timeout_ptr The timeout in 100 ns units, or null to wait indefinitely.
 * @return The wait status, such as X_STATUS_SUCCESS or X_STATUS_TIMEOUT.
 */
REX_RUNTIME_API u32 KeWaitForSingleObject(u32 object_address, u32 wait_reason, u32 processor_mode,
                                          u32 alertable, u64* timeout_ptr);
/**
 * Signals an event object in guest memory.
 *
 * @param event_address Guest address of the event.
 * @param increment Priority boost, ignored.
 * @param wait Whether the caller waits next, ignored.
 * @return The previous signal state, or 0 when the address is not an event.
 */
REX_RUNTIME_API u32 KeSetEvent(u32 event_address, u32 increment, u32 wait);

/**
 * Acquires a spinlock stored in guest memory.
 *
 * @param memory The guest memory.
 * @param ctx The calling context, whose r13 addresses the PCR.
 * @param lock The spinlock in guest memory.
 * @param change_irql True to raise the IRQL while the lock is held.
 * @return The previous IRQL.
 */
REX_RUNTIME_API u32 KeKfAcquireSpinLock(rex::runtime::Memory& memory, PPCContext* ctx,
                                        rex::X_KSPINLOCK* lock, bool change_irql = true);
/**
 * Releases a spinlock stored in guest memory.
 *
 * @param memory The guest memory.
 * @param ctx The calling context, whose r13 addresses the PCR.
 * @param lock The spinlock in guest memory.
 * @param old_irql The IRQL returned by KeKfAcquireSpinLock.
 * @param change_irql True to restore the IRQL.
 */
REX_RUNTIME_API void KeKfReleaseSpinLock(rex::runtime::Memory& memory, PPCContext* ctx,
                                         rex::X_KSPINLOCK* lock, u32 old_irql,
                                         bool change_irql = true);

/**
 * Initializes an APC object in guest memory.
 *
 * @param apc The APC to initialize.
 * @param thread_ptr Guest address of the target thread.
 * @param kernel_routine Guest address of the kernel routine.
 * @param rundown_routine Guest address of the rundown routine.
 * @param normal_routine Guest address of the normal routine, or 0 for a kernel APC.
 * @param apc_mode The processor mode, used only when a normal routine is given.
 * @param normal_context Context passed to the normal routine, used only when one is given.
 */
REX_RUNTIME_API void KeInitializeApc(rex::runtime::XAPC* apc, u32 thread_ptr, u32 kernel_routine,
                                     u32 rundown_routine, u32 normal_routine, u32 apc_mode,
                                     u32 normal_context);
/**
 * Queues an APC on its target thread.
 *
 * @param memory The guest memory.
 * @param apc The APC to queue.
 * @param arg1 First system argument passed to the APC routines.
 * @param arg2 Second system argument passed to the APC routines.
 * @param priority_increment Priority boost, ignored.
 * @param ctx The calling context, whose r13 addresses the PCR.
 * @return 1 when queued, 0 when it has no thread, is already queued or is refused.
 */
REX_RUNTIME_API u32 KeInsertQueueApc(rex::runtime::Memory& memory, rex::runtime::XAPC* apc,
                                     u32 arg1, u32 arg2, u32 priority_increment, PPCContext* ctx);

}  // namespace rex::kernel::xboxkrnl
