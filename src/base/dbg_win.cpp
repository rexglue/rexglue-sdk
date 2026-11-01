/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "platform_win.h"

#include <rex/platform/process.h>
#include <rex/string/text.h>

namespace rex::platform {

bool IsDebuggerAttached() {
  return IsDebuggerPresent() ? true : false;
}

void Break() {
  __debugbreak();
}

}  // namespace rex::platform
