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

#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr);
X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr,
                                                    uint32_t spin_count);

}  // namespace rex::kernel::xboxkrnl
