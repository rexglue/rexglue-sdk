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

#include <rex/kernel/xboxkrnl.h>
#include <rex/logging/log_category.h>
#include "kernel/categories.h"
#include <rex/math.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/kernel_thread.h>

namespace rex::kernel::xboxkrnl {

u32 RtlNtStatusToDosError_entry(u32 source_status) {
  u32 result = RtlNtStatusToDosError(source_status);
  REXKRNL_TRACE(
      "RtlNtStatusToDosError "
      "status={:#x} -> {}",
      source_status, result);
  return result;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__RtlNtStatusToDosError, rex::kernel::xboxkrnl::RtlNtStatusToDosError_entry)
