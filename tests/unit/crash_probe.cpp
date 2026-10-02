/**
 * @file        tests/unit/crash_probe.cpp
 * @brief       Child process that crashes in a named way for the crash reporter tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/crash.h>
#include <rex/platform/exceptions.h>
#include <rex/runtime/xmemory.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace {

[[gnu::noinline]] void WriteNull() {
  *static_cast<volatile int*>(nullptr) = 1;
}

[[gnu::noinline]] int Recurse(int depth) {
  volatile char buffer[4096];
  buffer[0] = static_cast<char>(depth);
  if (depth < 0) {
    return buffer[0];
  }
  return Recurse(depth + 1) + buffer[0];
}

[[noreturn, gnu::noinline]] void Throw() {
  throw std::runtime_error("probe terminate");
}

[[noreturn]] void ThrowFromNoexcept() noexcept {
  Throw();
}

std::string DescribeForProbe(rex::u64 address) {
  return "probe describer " + std::to_string(address);
}

std::pair<rex::u32, rex::u32> IgnoreInvalidation(void*, rex::u32, rex::u32, bool) {
  return {0, UINT32_MAX};
}

int Run(std::string_view mode) {
  if (mode == "null-write") {
    WriteNull();
  } else if (mode == "terminate") {
    ThrowFromNoexcept();
  } else if (mode == "abort") {
    std::abort();
  } else if (mode == "stack-overflow") {
    return Recurse(0);
  } else if (mode == "mmio") {
    rex::runtime::Memory memory;
    if (!memory.Initialize()) {
      return 2;
    }
    *memory.TranslateVirtual<volatile rex::u32*>(0x7FC80010) = 1;
  } else if (mode == "seh") {
    rex::initialize_seh();
    bool caught = false;
    SEH_TRY {
      WriteNull();
    }
    SEH_CATCH_ALL {
      caught = true;
    }
    SEH_END
    std::puts(caught ? "seh caught" : "seh missed");
    return caught ? 0 : 3;
  } else if (mode == "watch") {
    rex::runtime::Memory memory;
    if (!memory.Initialize()) {
      return 2;
    }
    const rex::u32 address =
        memory.SystemHeapAlloc(0x1000, 0x1000, rex::runtime::kSystemHeapPhysical);
    void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(IgnoreInvalidation, nullptr);
    memory.EnablePhysicalMemoryAccessCallbacks(memory.GetPhysicalAddress(address), 0x1000, true,
                                               false);
    *memory.TranslateVirtual<volatile rex::u32*>(address) = 0x12345678;
    memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
    std::puts("watch resumed");
    return 0;
  } else if (mode == "foreign-thread") {
    std::thread([] { WriteNull(); }).join();
  } else if (mode == "describer") {
    rex::platform::SetCrashAddressDescriber(DescribeForProbe);
    WriteNull();
  }
  return 4;
}

}  // namespace

int main(int argc, char** argv) {
#if REX_PLATFORM_WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  if (argc < 2) {
    return 5;
  }
  rex::LogConfig config;
  config.console = true;
  config.debugger = false;
  config.flush_interval = std::chrono::seconds(0);
  if (!rex::log::Start(config)) {
    return 6;
  }
  if (!rex::platform::InstallCrashReporter({.app_name = "rex-crash-probe"})) {
    return 7;
  }
  return Run(argv[1]);
}
