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
#include <rex/runtime/xtypes.h>

namespace rex::kernel::xboxkrnl {

/**
 * Converts an NT status code to the matching Win32 error code.
 *
 * @param source_status The NT status code.
 * @return The Win32 error code, or 317 when the status has no mapping.
 */
REX_RUNTIME_API uint32_t RtlNtStatusToDosError(uint32_t source_status);

}  // namespace rex::kernel::xboxkrnl
