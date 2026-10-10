/**
 * @file        tests/unit/memory/access_violation_test.cpp
 * @brief       Register block reservation and the access violation path of Memory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <cstdint>
#include <utility>

#include <rex/exception_handler.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/runtime/xmemory.h>

#include "test_memory.h"

using rex::runtime::AccessViolationResult;

REX_TEST_CASE("Memory reserves both register blocks as no access", "[memory][mmio]") {
  auto& memory = rex::testing::GetTestMemory();
  for (rex::u32 address : {0x7FC80000u, 0x7FC8FFFCu, 0x7FEA0000u, 0x7FEAFFFCu}) {
    size_t length = rex::memory::page_size();
    rex::memory::PageAccess access = rex::memory::PageAccess::kReadWrite;
    REQUIRE(rex::memory::QueryProtect(memory.TranslateVirtual(address), length, access));
    CHECK(access == rex::memory::PageAccess::kNoAccess);
  }
}

REX_TEST_CASE("A fault in a register block is an MMIO miss", "[memory][mmio]") {
  auto& memory = rex::testing::GetTestMemory();
  CHECK(memory.HandleAccessViolation(memory.TranslateVirtual(0x7FC80010), false) ==
        AccessViolationResult::MMIOMiss);
  CHECK(memory.HandleAccessViolation(memory.TranslateVirtual(0x7FEAFFFC), true) ==
        AccessViolationResult::MMIOMiss);
}

REX_TEST_CASE("A fault outside guest memory is unhandled", "[memory][mmio]") {
  auto& memory = rex::testing::GetTestMemory();
  int local = 0;
  CHECK(memory.HandleAccessViolation(&local, true) == AccessViolationResult::Unhandled);
}

#if REX_PLATFORM_WIN32

namespace {

std::atomic<int> g_invalidations{0};

std::pair<rex::u32, rex::u32> CountInvalidation(void*, rex::u32, rex::u32, bool) {
  g_invalidations.fetch_add(1);
  return {0, UINT32_MAX};
}

struct BlockProbe {
  rex::u8* page = nullptr;
  size_t page_size = 0;
  bool reached = false;
};

bool OpenBlockPage(rex::platform::Exception* ex, void* data) {
  auto* probe = static_cast<BlockProbe*>(data);
  const auto fault = static_cast<uintptr_t>(ex->fault_address());
  const auto page = reinterpret_cast<uintptr_t>(probe->page);
  if (fault < page || fault >= page + probe->page_size) {
    return false;
  }
  probe->reached = true;
  return rex::memory::Protect(probe->page, probe->page_size, rex::memory::PageAccess::kReadOnly,
                              nullptr);
}

}  // namespace

REX_TEST_CASE("A write to a watched physical page runs the invalidation callback and resumes",
              "[memory][mmio]") {
  auto& memory = rex::testing::GetTestMemory();
  const rex::u32 address =
      memory.SystemHeapAlloc(0x1000, 0x1000, rex::runtime::kSystemHeapPhysical);
  REQUIRE(address != 0);
  const rex::u32 physical = memory.GetPhysicalAddress(address);
  REQUIRE(physical != UINT32_MAX);
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(CountInvalidation, nullptr);
  memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
  g_invalidations = 0;
  *memory.TranslateVirtual<volatile rex::u32*>(address) = 0x12345678;
  CHECK(g_invalidations.load() > 0);
  CHECK(*memory.TranslateVirtual<volatile rex::u32*>(address) == 0x12345678u);
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
  memory.SystemHeapFree(address);
}

REX_TEST_CASE("A plain read of a register block is refused by Memory and reaches the next handler",
              "[memory][mmio]") {
  auto& memory = rex::testing::GetTestMemory();
  BlockProbe probe{memory.TranslateVirtual(0x7FC80000), rex::memory::page_size()};
  rex::platform::ExceptionHandler::Install(OpenBlockPage, &probe);
  const rex::u32 value = *memory.TranslateVirtual<volatile rex::u32*>(0x7FC80010);
  rex::platform::ExceptionHandler::Uninstall(OpenBlockPage, &probe);
  (void)value;
  CHECK(probe.reached);
  CHECK(rex::memory::Protect(probe.page, probe.page_size, rex::memory::PageAccess::kNoAccess,
                             nullptr));
}

#endif
