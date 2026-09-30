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

#include <rex/platform/mutex.h>

namespace rex::platform {

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

}  // namespace rex::platform
