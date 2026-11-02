/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/xboxkrnl.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <cstring>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/thread.h>
#include <rex/ppc/context.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/list.h>
#include <rex/memory.h>
#include <rex/title.h>

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

REX_KERNEL_API void RtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs);
/**
 * Initializes a critical section in guest memory with a spin count.
 *
 * @param spin_count The spin count, stored in units of 256 and capped at 255 units.
 * @return X_STATUS_SUCCESS.
 */
REX_KERNEL_API X_STATUS RtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs,
                                                                 u32 spin_count);
}  // namespace rex::kernel::xboxkrnl

namespace rex::kernel::xboxkrnl {

/**
 * Signals an event.
 *
 * @param previous_state_ptr Receives the previous signal state, or null to skip.
 * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE when the handle is not an event.
 */
REX_KERNEL_API u32 NtSetEvent(u32 handle, be_u32* previous_state_ptr);
/**
 * Sets an event to the nonsignaled state.
 *
 * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE when the handle is not an event.
 */
REX_KERNEL_API u32 NtClearEvent(u32 handle);

/**
 * Waits on several handles at once.
 *
 * @param wait_type 0 to wait for all handles, 1 to wait for any handle.
 * @param alertable Nonzero when a user APC can interrupt the wait.
 * @param timeout_ptr The timeout in 100 ns units, or null to wait indefinitely.
 * @return The wait status, such as the index of the signaled handle or X_STATUS_TIMEOUT.
 */
REX_KERNEL_API u32 NtWaitForMultipleObjectsEx(u32 count, be_u32* handles, u32 wait_type,
                                              u32 wait_mode, u32 alertable, u64* timeout_ptr);

/**
 * Waits on a kernel object in guest memory.
 *
 * @param alertable Nonzero when a user APC can interrupt the wait.
 * @param timeout_ptr The timeout in 100 ns units, or null to wait indefinitely.
 * @return The wait status, such as X_STATUS_SUCCESS or X_STATUS_TIMEOUT.
 */
REX_KERNEL_API u32 KeWaitForSingleObject(u32 object_address, u32 wait_reason, u32 processor_mode,
                                         u32 alertable, u64* timeout_ptr);
/**
 * Signals an event object in guest memory.
 *
 * @param increment Priority boost, ignored.
 * @param wait Whether the caller waits next, ignored.
 * @return The previous signal state, or 0 when the address is not an event.
 */
REX_KERNEL_API u32 KeSetEvent(u32 event_address, u32 increment, u32 wait);

/**
 * Acquires a spinlock stored in guest memory.
 *
 * @param ctx The calling context, whose r13 addresses the PCR.
 * @param change_irql True to raise the IRQL while the lock is held.
 * @return The previous IRQL.
 */
REX_KERNEL_API u32 KeKfAcquireSpinLock(rex::kernel::Memory& memory, PPCContext* ctx,
                                       rex::kernel::X_KSPINLOCK* lock, bool change_irql = true);
/**
 * Releases a spinlock stored in guest memory.
 *
 * @param ctx The calling context, whose r13 addresses the PCR.
 * @param old_irql The IRQL returned by KeKfAcquireSpinLock.
 * @param change_irql True to restore the IRQL.
 */
REX_KERNEL_API void KeKfReleaseSpinLock(rex::kernel::Memory& memory, PPCContext* ctx,
                                        rex::kernel::X_KSPINLOCK* lock, u32 old_irql,
                                        bool change_irql = true);

/**
 * Initializes an APC object in guest memory.
 *
 * @param apc The APC to initialize.
 * @param normal_routine Guest address of the normal routine, or 0 for a kernel APC.
 * @param apc_mode The processor mode, used only when a normal routine is given.
 * @param normal_context Context passed to the normal routine, used only when one is given.
 */
REX_KERNEL_API void KeInitializeApc(rex::kernel::XAPC* apc, u32 thread_ptr, u32 kernel_routine,
                                    u32 rundown_routine, u32 normal_routine, u32 apc_mode,
                                    u32 normal_context);
/**
 * Queues an APC on its target thread.
 *
 * @param apc The APC to queue.
 * @param arg1 First system argument passed to the APC routines.
 * @param arg2 Second system argument passed to the APC routines.
 * @param priority_increment Priority boost, ignored.
 * @param ctx The calling context, whose r13 addresses the PCR.
 * @return 1 when queued, 0 when the APC has no thread, is already queued or its thread takes no
 * APCs.
 */
REX_KERNEL_API u32 KeInsertQueueApc(rex::kernel::Memory& memory, rex::kernel::XAPC* apc, u32 arg1,
                                    u32 arg2, u32 priority_increment, PPCContext* ctx);

}  // namespace rex::kernel::xboxkrnl

namespace rex::kernel {

#pragma pack(push, 4)

struct X_VIDEO_MODE {
  be_u32 display_width;
  be_u32 display_height;
  be_u32 is_interlaced;  /**< Nonzero for an interlaced mode. */
  be_u32 is_widescreen;  /**< Nonzero for a widescreen mode. */
  be_u32 is_hi_def;      /**< Nonzero for a high definition mode. */
  be_f32 refresh_rate;   /**< Refresh rate in hertz. */
  be_u32 video_standard; /**< Video standard of the display. The query writes 1. */
  be_u32 reserved_0;     /**< Inside the XDK Reserved array. The query writes 0x4A. */
  be_u32 unknown_0x01;   /**< Inside the XDK Reserved array. The query writes 0x01. */
  be_u32 reserved[3];
};
REX_STATIC_ASSERT_SIZE(X_VIDEO_MODE, 48);

/**
 * Fills a guest video mode structure from a display mode.
 *
 * @param out Receives the guest structure.
 */
inline void FillXVideoMode(const VideoMode& mode, X_VIDEO_MODE& out) {
  std::memset(&out, 0, sizeof(X_VIDEO_MODE));
  out.display_width = mode.width;
  out.display_height = mode.height;
  out.is_interlaced = 0;
  out.is_widescreen = mode.width * 3 >= mode.height * 4;
  out.is_hi_def = mode.width >= 1280 || mode.height >= 720;
  out.refresh_rate = mode.refresh_rate;
  out.video_standard = 1;
  out.reserved_0 = 0x4A;
  out.unknown_0x01 = 0x01;
}

#pragma pack(pop)

}  // namespace rex::kernel

namespace rex::kernel::xboxkrnl {

/**
 * Fills a video mode structure from the runtime display settings.
 *
 * @param video_mode Receives the video mode.
 */
REX_KERNEL_API void VdQueryVideoMode(kernel::X_VIDEO_MODE* video_mode);

}  // namespace rex::kernel::xboxkrnl
