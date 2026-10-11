/**
 * @file        tests/unit/crash_probe_recompiled_code.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/platform/exceptions.h>
#include <rex/ppc/recompiled_code.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

REX_RECOMPILED_CODE_SECTION extern "C" void ProbeFaultInRecompiledCode(volatile int* address) {
  *address = 1;
}

namespace {
void (*volatile g_fault_in_recompiled_code)(volatile int*) = &ProbeFaultInRecompiledCode;
}  // namespace

REX_RECOMPILED_CODE_SECTION extern "C" int ProbeFaultInScope(volatile int* address) {
  int caught = 0;
  REX_SEH_TRY {
    *address = 1;
  }
  REX_SEH_CATCH_ALL {
    caught = 1;
  }
  REX_SEH_END
  return caught;
}

namespace {

void ProbeHostFrame(volatile int* address) {
#if !REX_PLATFORM_WIN32
  const rex::platform::HostCallDepth depth;
#endif
  g_fault_in_recompiled_code(address);
}

void (*volatile g_host_frame)(volatile int*) = &ProbeHostFrame;

}  // namespace

REX_RECOMPILED_CODE_SECTION extern "C" int ProbeFaultBelowHostFrame(volatile int* address) {
  int caught = 0;
  REX_SEH_TRY {
    g_host_frame(address);
  }
  REX_SEH_CATCH_ALL {
    caught = 1;
  }
  REX_SEH_END
  return caught;
}
