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

#include <rex/system/xevent.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {

uint32_t xeNtSetEvent(uint32_t handle, rex::be<uint32_t>* previous_state_ptr);
uint32_t xeNtClearEvent(uint32_t handle);

uint32_t xeNtWaitForMultipleObjectsEx(uint32_t count, rex::be<uint32_t>* handles,
                                      uint32_t wait_type, uint32_t wait_mode, uint32_t alertable,
                                      uint64_t* timeout_ptr);

uint32_t xeKeWaitForSingleObject(void* object_ptr, uint32_t wait_reason, uint32_t processor_mode,
                                 uint32_t alertable, uint64_t* timeout_ptr);
uint32_t xeKeSetEvent(rex::system::X_KEVENT* event_ptr, uint32_t increment, uint32_t wait);

}  // namespace rex::kernel::xboxkrnl
