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

#include <rex/dbg.h>
#include <rex/string/buffer.h>

namespace rex::debug {

bool IsDebuggerAttached() {
  return IsDebuggerPresent() ? true : false;
}

void Break() {
  __debugbreak();
}

namespace detail {
void DebugPrint(const char* s) {
  OutputDebugStringA(s);
}
}  // namespace detail

}  // namespace rex::debug
