/**
 * @file        tests/unit/kernel/access_violation_test.cpp
 * @brief       Register block reservation and the access violation path of GuestAddressSpace
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

#include <rex/platform/exception_handler.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform.h>
#include <rex/kernel/heap.h>
#include <rex/utility.h>

#include "test_memory.h"

using rex::kernel::AccessViolationResult;

REX_TEST_CASE("GuestAddressSpace reserves both register blocks as no access", "[kernel][mmio]") {
  auto& memory = rex::test::GetTestMemory();
  for (rex::u32 address : {0x7FC80000u, 0x7FC8FFFCu, 0x7FEA0000u, 0x7FEAFFFCu}) {
    size_t length = rex::memory::GetPageSize();
    rex::memory::PageAccess access = rex::memory::PageAccess::kReadWrite;
    REQUIRE(rex::memory::QueryProtect(memory.TranslateVirtual(address), length, access));
    CHECK(access == rex::memory::PageAccess::kNoAccess);
  }
}

REX_TEST_CASE("A fault in a register block is an MMIO miss", "[kernel][mmio]") {
  auto& memory = rex::test::GetTestMemory();
  CHECK(memory.HandleAccessViolation(memory.TranslateVirtual(0x7FC80010), false) ==
        AccessViolationResult::kMMIOMiss);
  CHECK(memory.HandleAccessViolation(memory.TranslateVirtual(0x7FEAFFFC), true) ==
        AccessViolationResult::kMMIOMiss);
}

REX_TEST_CASE("A fault outside guest memory is unhandled", "[kernel][mmio]") {
  auto& memory = rex::test::GetTestMemory();
  int local = 0;
  CHECK(memory.HandleAccessViolation(&local, true) == AccessViolationResult::kUnhandled);
}

namespace {

std::atomic<int> g_watch_hits{0};

std::pair<rex::u32, rex::u32> CountWatchHit(void*, rex::u32, rex::u32, bool) {
  g_watch_hits.fetch_add(1);
  return {0, UINT32_MAX};
}

}  // namespace

REX_TEST_CASE("A write to a watched host page unprotects it by the access of every guest page",
              "[kernel][mmio]") {
  const auto host_page = static_cast<rex::u32>(rex::memory::GetPageSize());
  constexpr rex::u32 kGuestPage = 0x1000;
  if (host_page <= kGuestPage) {
    SKIP("the host page is not larger than a guest page");
  }
  auto& memory = rex::test::GetTestMemory();
  const rex::u32 address =
      memory.SystemHeapAlloc(host_page, host_page, rex::kernel::kSystemHeapPhysical);
  REQUIRE(address != 0);
  auto* host = memory.TranslateVirtual<rex::u8*>(address);
  REQUIRE((reinterpret_cast<uintptr_t>(host) & (host_page - 1)) == 0);
  auto* heap = memory.LookupHeap(address);
  void* handle = nullptr;
  const rex::ScopeGuard cleanup([&] {
    heap->Protect(address, host_page,
                  rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
    if (handle) {
      memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
    }
    memory.SystemHeapFree(address);
  });
  REQUIRE(heap->Protect(address, kGuestPage, rex::kernel::kMemoryProtectRead));

  handle = memory.RegisterPhysicalMemoryInvalidationCallback(CountWatchHit, nullptr);
  g_watch_hits = 0;
  memory.EnablePhysicalMemoryAccessCallbacks(memory.GetPhysicalAddress(address), host_page, true,
                                             false);
  size_t length = host_page;
  rex::memory::PageAccess access = rex::memory::PageAccess::kReadWrite;
  REQUIRE(rex::memory::QueryProtect(host, length, access));
  CHECK(access == rex::memory::PageAccess::kReadOnly);

  CHECK(memory.HandleAccessViolation(host + kGuestPage, true) == AccessViolationResult::kResumed);
  CHECK(g_watch_hits.load() == 1);
  length = host_page;
  REQUIRE(rex::memory::QueryProtect(host, length, access));
  CHECK(access == rex::memory::PageAccess::kReadWrite);

  *reinterpret_cast<volatile rex::u32*>(host + kGuestPage) = 0x12345678;
  CHECK(g_watch_hits.load() == 1);
}

#if REX_PLATFORM_WIN32

namespace {

std::atomic<int> g_invalidations{0};

std::pair<rex::u32, rex::u32> CountInvalidation(void*, rex::u32, rex::u32, bool) {
  g_invalidations.fetch_add(1);
  return {0, UINT32_MAX};
}

struct BlockFault {
  rex::u8* page = nullptr;
  size_t page_size = 0;
  bool reached = false;
};

bool OpenBlockPage(rex::platform::Exception* ex, void* data) {
  auto* fault_block = static_cast<BlockFault*>(data);
  const auto fault = static_cast<uintptr_t>(ex->GetFaultAddress());
  const auto page = reinterpret_cast<uintptr_t>(fault_block->page);
  if (fault < page || fault >= page + fault_block->page_size) {
    return false;
  }
  fault_block->reached = true;
  return rex::memory::Protect(fault_block->page, fault_block->page_size,
                              rex::memory::PageAccess::kReadOnly, nullptr);
}

}  // namespace

REX_TEST_CASE("A write to a watched physical page runs the invalidation callback and resumes",
              "[kernel][mmio]") {
  auto& memory = rex::test::GetTestMemory();
  const rex::u32 address = memory.SystemHeapAlloc(0x1000, 0x1000, rex::kernel::kSystemHeapPhysical);
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

REX_TEST_CASE(
    "A plain read of a register block is refused by GuestAddressSpace and reaches the next handler",
    "[kernel][mmio]") {
  auto& memory = rex::test::GetTestMemory();
  BlockFault fault_block{memory.TranslateVirtual(0x7FC80000), rex::memory::GetPageSize()};
  rex::platform::ExceptionHandler::Install(OpenBlockPage, &fault_block);
  const rex::u32 value = *memory.TranslateVirtual<volatile rex::u32*>(0x7FC80010);
  rex::platform::ExceptionHandler::Uninstall(OpenBlockPage, &fault_block);
  (void)value;
  CHECK(fault_block.reached);
  CHECK(rex::memory::Protect(fault_block.page, fault_block.page_size,
                             rex::memory::PageAccess::kNoAccess, nullptr));
}

#endif
