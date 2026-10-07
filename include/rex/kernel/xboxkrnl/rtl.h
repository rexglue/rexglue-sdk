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

#include <rex/macros.h>
#include <rex/runtime/xtypes.h>

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

REX_RUNTIME_API void RtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr);
REX_RUNTIME_API X_STATUS RtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs,
                                                                  uint32_t cs_ptr,
                                                                  uint32_t spin_count);

}  // namespace rex::kernel::xboxkrnl
