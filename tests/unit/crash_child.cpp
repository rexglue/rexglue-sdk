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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
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
#include <rex/ppc/detour.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/stack.h>
#include <rex/runtime/xmemory.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

extern "C" int FaultInScope(volatile int* address);
extern "C" int FaultBelowHostFrame(volatile int* address);

alignas(64) static rex::u8 g_detour_memory[0x1000] = {};

extern "C" void __imp__CrashChildDetourSkip(PPCContext& ctx, rex::u8*) {
  ctx.r3.u64 = 1;
}

REX_DETOUR(CrashChildDetourSkip, void, (rex::u32 value)) {
  (void)value;
}

extern "C" void __imp__CrashChildDetourOpen(PPCContext& ctx, rex::u8*) {
  ctx.r3.u64 = 1;
}

REX_DETOUR(CrashChildDetourOpen, void, (PPCContext & ctx)) {
  auto* frame = new rex::ppc::StackFrame(ctx, g_detour_memory);
  frame->Push<rex::u32>(1);
  original(ctx);
}

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

std::atomic<int> g_invalidations{0};

std::pair<rex::u32, rex::u32> CountInvalidation(void*, rex::u32 address, rex::u32 length, bool) {
  g_invalidations.fetch_add(1);
  return {address, length};
}

std::optional<unsigned long long> ReadSyscallCount() {
  std::ifstream io("/proc/self/io");
  std::string key;
  unsigned long long value = 0;
  while (io >> key >> value) {
    if (key == "syscr:") {
      return value;
    }
  }
  return std::nullopt;
}

int RunWatchBurst() {
  rex::runtime::Memory memory;
  if (!memory.Initialize()) {
    return 2;
  }
  constexpr rex::u32 kPages = 100;
  const auto host_page = static_cast<rex::u32>(rex::memory::GetPageSize());
  const rex::u32 address =
      memory.SystemHeapAlloc(host_page * kPages, host_page, rex::runtime::kSystemHeapPhysical);
  if (!address) {
    return 2;
  }
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(CountInvalidation, nullptr);
  memory.EnablePhysicalMemoryAccessCallbacks(memory.GetPhysicalAddress(address), host_page * kPages,
                                             true, false);
  g_invalidations = 0;
  const auto before = ReadSyscallCount();
  for (rex::u32 page = 0; page < kPages; ++page) {
    *memory.TranslateVirtual<volatile rex::u32*>(address + page * host_page) = page;
  }
  const auto after = ReadSyscallCount();
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
  if (g_invalidations.load() != static_cast<int>(kPages)) {
    std::printf("watch burst fired %d times\n", g_invalidations.load());
    return 3;
  }
  if (!before || !after) {
    std::puts("watch burst skipped");
  } else if (*after - *before >= 20) {
    std::printf("watch burst read %llu times\n", *after - *before);
    return 3;
  } else {
    std::puts("watch burst ok");
  }
  return 0;
}

int RunWatchRace() {
  rex::runtime::Memory memory;
  if (!memory.Initialize()) {
    return 2;
  }
  const auto host_page = static_cast<rex::u32>(rex::memory::GetPageSize());
  const rex::u32 address =
      memory.SystemHeapAlloc(host_page, host_page, rex::runtime::kSystemHeapPhysical);
  if (!address) {
    return 2;
  }
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(CountInvalidation, nullptr);
  g_invalidations = 0;
  constexpr int kRounds = 300;
  std::atomic<int> round{0};
  std::atomic<int> finished{0};
  auto* host = memory.TranslateVirtual<volatile rex::u32*>(address);
  const auto writer = [&](rex::u32 value) {
    for (int next = 1; next <= kRounds; ++next) {
      while (round.load(std::memory_order_acquire) < next) {
        std::this_thread::yield();
      }
      *host = value;
      finished.fetch_add(1);
    }
  };
  std::thread first(writer, 1);
  std::thread second(writer, 2);
  const rex::u32 physical = memory.GetPhysicalAddress(address);
  for (int next = 1; next <= kRounds; ++next) {
    memory.EnablePhysicalMemoryAccessCallbacks(physical, host_page, true, false);
    round.store(next, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (finished.load() < next * 2) {
      if (std::chrono::steady_clock::now() > deadline) {
        std::puts("watch race hang");
        std::fflush(stdout);
        std::_Exit(3);
      }
      std::this_thread::yield();
    }
  }
  first.join();
  second.join();
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
  if (g_invalidations.load() != kRounds) {
    std::printf("watch race fired %d times\n", g_invalidations.load());
    return 3;
  }
  std::puts("watch race ok");
  return 0;
}

int RunStackFramesOutOfOrder() {
  alignas(64) static rex::u8 memory[0x1000] = {};
  PPCContext ctx{};
  ctx.r1.u32 = 0x800;
  auto outer = std::make_unique<rex::ppc::StackFrame>(ctx, memory);
  auto inner = std::make_unique<rex::ppc::StackFrame>(ctx, memory);
  outer.reset();
  inner.reset();
  std::puts("stack frames closed out of order");
  return 0;
}

int RunStackPushEmptyBytes() {
  alignas(64) static rex::u8 memory[0x1000] = {};
  PPCContext ctx{};
  ctx.r1.u32 = 0x800;
  rex::ppc::StackFrame frame(ctx, memory);
  frame.PushBytes({});
  std::puts("stack pushed empty bytes");
  return 0;
}

int RunDetourSkipsOriginal() {
  PPCContext ctx{};
  ctx.r1.u32 = 0x800;
  try {
    CrashChildDetourSkip(ctx, g_detour_memory);
  } catch (...) {
    return 3;
  }
  std::puts("detour returned without its original");
  return 0;
}

int RunDetourLeavesFrameOpen() {
  PPCContext ctx{};
  ctx.r1.u32 = 0x800;
  try {
    CrashChildDetourOpen(ctx, g_detour_memory);
  } catch (...) {
    return 3;
  }
  std::puts("detour returned with a frame open");
  return 0;
}

int RunStackPushPastLimit() {
  alignas(64) static rex::u8 memory[0x1000] = {};
  PPCContext ctx{};
  ctx.r13.u32 = 0x100;
  *rex::runtime::GuestPtr<be_u32*>(memory, 0x170) = 0x800;
  *rex::runtime::GuestPtr<be_u32*>(memory, 0x174) = 0x400;
  ctx.r1.u32 = 0x800;
  rex::ppc::StackFrame frame(ctx, memory);
  frame.Push<u32>(1);
  std::puts("pushed above the limit");
  std::fflush(stdout);
  const std::array<rex::u8, 0x500> bytes{};
  frame.PushBytes(bytes);
  std::puts("pushed below the limit");
  return 0;
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
  } else if (mode == "watch-burst") {
    return RunWatchBurst();
  } else if (mode == "watch-race") {
    return RunWatchRace();
  } else if (mode == "stack-frames-out-of-order") {
    return RunStackFramesOutOfOrder();
  } else if (mode == "stack-push-past-limit") {
    return RunStackPushPastLimit();
  } else if (mode == "stack-push-empty-bytes") {
    return RunStackPushEmptyBytes();
  } else if (mode == "detour-skips-original") {
    return RunDetourSkipsOriginal();
  } else if (mode == "detour-leaves-frame-open") {
    return RunDetourLeavesFrameOpen();
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
