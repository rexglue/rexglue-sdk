/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime/xtypes.h>

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

/**
 * Initializes a critical section in guest memory.
 *
 * @param cs The critical section to initialize.
 */
REX_RUNTIME_API void RtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs);
/**
 * Initializes a critical section in guest memory with a spin count.
 *
 * @param cs The critical section to initialize.
 * @param spin_count The spin count, stored in units of 256 and capped at 255 units.
 * @return X_STATUS_SUCCESS.
 */
REX_RUNTIME_API X_STATUS RtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs,
                                                                  u32 spin_count);

}  // namespace rex::kernel::xboxkrnl
