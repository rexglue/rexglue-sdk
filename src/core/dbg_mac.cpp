/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime
 */

#include <csignal>
#include <iostream>
#include <mutex>

#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/dbg.h>

namespace rex::debug {

bool IsDebuggerAttached() {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  kinfo_proc process_info = {};
  size_t process_info_size = sizeof(process_info);
  if (sysctl(mib, 4, &process_info, &process_info_size, nullptr, 0) != 0 ||
      process_info_size < sizeof(process_info)) {
    return false;
  }
  return (process_info.kp_proc.p_flag & P_TRACED) != 0;
}

void Break() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    // Install handler for sigtrap only once
    std::signal(SIGTRAP, [](int) {
      // Forward signal to default handler after being caught
      std::signal(SIGTRAP, SIG_DFL);
    });
  });
  std::raise(SIGTRAP);
}

namespace detail {
void DebugPrint(const char* s) {
  std::clog << s << std::endl;
}
}  // namespace detail

}  // namespace rex::debug
