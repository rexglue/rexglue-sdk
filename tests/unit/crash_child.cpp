/**
 * @file        tests/unit/crash_child.cpp
 * @brief       Child process that crashes in a named way for the crash reporter tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <span>
#include <utility>

#include <rex/logging/config.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform.h>
#include <rex/platform/crash.h>
#include <rex/platform/exceptions.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/runtime/xmemory.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

extern "C" int FaultInScope(volatile int* address);
extern "C" int FaultBelowHostFrame(volatile int* address);

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
  throw std::runtime_error("crash child terminate");
}

// NOLINTNEXTLINE(bugprone-exception-escape): the test needs the throw to reach std::terminate
[[noreturn]] void ThrowFromNoexcept() noexcept {
  Throw();
}

rex::u32 DescribeForCrashChild(rex::u64 address, std::span<char> out) {
  return static_cast<rex::u32>(std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()),
                                                "crash child describer {}", address)
                                   .size);
}

std::atomic<bool> g_fault_other_thread{false};

rex::u32 DescribeWhileAnotherThreadFaults(rex::u64, std::span<char> out) {
  g_fault_other_thread = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  constexpr std::string_view kText = "crash child concurrent describer";
  const auto end = std::ranges::copy(kText.substr(0, out.size()), out.begin()).out;
  return static_cast<rex::u32>(end - out.begin());
}

std::pair<rex::u32, rex::u32> IgnoreInvalidation(void*, rex::u32, rex::u32, bool) {
  return {0, UINT32_MAX};
}

int Run(std::string_view mode) {
  if (mode == "null-write") {
    WriteNull();
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
  } else if (mode == "read-only-write") {
    rex::runtime::Memory memory;
    if (!memory.Initialize() || !memory.LookupHeap(0x40000000)
                                     ->AllocFixed(0x40000000, 0x10000, 0x10000,
                                                  rex::runtime::kMemoryAllocationReserve |
                                                      rex::runtime::kMemoryAllocationCommit,
                                                  rex::runtime::kMemoryProtectRead)) {
      return 2;
    }
    *memory.TranslateVirtual<volatile rex::u32*>(0x40000010) = 1;
  } else if (mode == "stale-protect") {
    {
      rex::runtime::Memory memory;
      if (!memory.Initialize()) {
        return 2;
      }
      const auto host_page = static_cast<rex::u32>(rex::memory::GetPageSize());
      const rex::u32 address =
          memory.SystemHeapAlloc(host_page, host_page, rex::runtime::kSystemHeapPhysical);
      auto* host = memory.TranslateVirtual<volatile rex::u32*>(address);
      // NOLINTNEXTLINE(performance-no-int-to-ptr): rounds the host pointer down to its page
      auto* page = reinterpret_cast<rex::u8*>(reinterpret_cast<rex::host_addr_t>(host) &
                                              ~rex::host_addr_t{host_page - 1});
      if (!address ||
          !rex::memory::Protect(page, host_page, rex::memory::PageAccess::kReadOnly, nullptr)) {
        return 2;
      }
      *host = 1;
      std::puts("stale resumed");
      std::fflush(stdout);
    }
    return 0;
  } else if (mode == "seh") {
    rex::InitializeSEH();
    rex::platform::AddRecompiledCode(REX_RECOMPILED_CODE());
    const bool caught = FaultInScope(nullptr) == 1;
    std::puts(caught ? "seh caught" : "seh missed");
    return caught ? 0 : 3;
  } else if (mode == "seh-host-frame") {
    rex::InitializeSEH();
    rex::platform::AddRecompiledCode(REX_RECOMPILED_CODE());
    const bool caught = FaultBelowHostFrame(nullptr) == 1;
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
    rex::platform::SetCrashAddressDescriber(DescribeForCrashChild);
    WriteNull();
  } else if (mode == "concurrent") {
    rex::platform::SetCrashAddressDescriber(DescribeWhileAnotherThreadFaults);
    std::thread([] {
      while (!g_fault_other_thread) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      WriteNull();
    }).detach();
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
  const std::string_view mode = argv[1];
  try {
    rex::LogConfig config;
    config.console = true;
    config.debugger = false;
    config.flush_interval = std::chrono::seconds(0);
    if (!rex::log::Start(config)) {
      return 6;
    }
    if (!rex::platform::InstallCrashReporter({.app_name = "rex-crash-child"})) {
      return 7;
    }
    if (mode != "terminate") {
      return Run(mode);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "crash child failed: %s\n", error.what());
    return 9;
  }
  ThrowFromNoexcept();
}
