/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <csignal>
#include <iostream>
#include <mutex>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <cstdarg>
#include <fstream>
#include <sstream>
#endif

#include <rex/dbg.h>

namespace rex::platform {

bool IsDebuggerAttached() {
#if defined(__APPLE__)
  // Darwin has no /proc; query the process's traced flag via sysctl.
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  kinfo_proc process_info = {};
  size_t process_info_size = sizeof(process_info);
  if (sysctl(mib, 4, &process_info, &process_info_size, nullptr, 0) != 0 ||
      process_info_size < sizeof(process_info)) {
    return false;
  }
  return (process_info.kp_proc.p_flag & P_TRACED) != 0;
#else
  std::ifstream proc_status_stream("/proc/self/status");
  if (!proc_status_stream.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(proc_status_stream, line)) {
    std::istringstream line_stream(line);
    std::string key;
    line_stream >> key;
    if (key == "TracerPid:") {
      uint32_t tracer_pid;
      line_stream >> tracer_pid;
      return tracer_pid != 0;
    }
  }
  return false;
#endif
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

}  // namespace rex::platform
