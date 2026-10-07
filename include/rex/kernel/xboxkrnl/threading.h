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
#include <rex/runtime/xevent.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {

REX_RUNTIME_API uint32_t NtSetEvent(uint32_t handle, rex::be<uint32_t>* previous_state_ptr);
REX_RUNTIME_API uint32_t NtClearEvent(uint32_t handle);

REX_RUNTIME_API uint32_t NtWaitForMultipleObjectsEx(uint32_t count, rex::be<uint32_t>* handles,
                                                    uint32_t wait_type, uint32_t wait_mode,
                                                    uint32_t alertable, uint64_t* timeout_ptr);

REX_RUNTIME_API uint32_t KeWaitForSingleObject(u32 object_address, uint32_t wait_reason,
                                               uint32_t processor_mode, uint32_t alertable,
                                               uint64_t* timeout_ptr);
REX_RUNTIME_API uint32_t KeSetEvent(u32 event_address, uint32_t increment, uint32_t wait);

// Guest-memory spinlock helpers (PPCContext* for r13/PCR access)
REX_RUNTIME_API uint32_t KeKfAcquireSpinLock(rex::runtime::Memory& memory, PPCContext* ctx,
                                             rex::X_KSPINLOCK* lock, bool change_irql = true);
REX_RUNTIME_API void KeKfReleaseSpinLock(rex::runtime::Memory& memory, PPCContext* ctx,
                                         rex::X_KSPINLOCK* lock, uint32_t old_irql,
                                         bool change_irql = true);

// Guest-memory APC helpers
REX_RUNTIME_API void KeInitializeApc(rex::runtime::XAPC* apc, uint32_t thread_ptr,
                                     uint32_t kernel_routine, uint32_t rundown_routine,
                                     uint32_t normal_routine, uint32_t apc_mode,
                                     uint32_t normal_context);
REX_RUNTIME_API uint32_t KeInsertQueueApc(rex::runtime::Memory& memory, rex::runtime::XAPC* apc,
                                          uint32_t arg1, uint32_t arg2, uint32_t priority_increment,
                                          PPCContext* ctx);

}  // namespace rex::kernel::xboxkrnl
