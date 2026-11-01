/**
 * @file        tests/unit/base/fixed_mapping_test.cpp
 * @brief       The fixed address mapping check for kernels that treat the no replace flag as a hint
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/platform.h>

#if REX_PLATFORM_LINUX

#include <cerrno>
#include <fstream>
#include <optional>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#include <rex/memory/virtual_memory.h>

#include "base/memory_posix.h"

namespace {

struct Mapping {
  void* address = MAP_FAILED;
  size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));

  Mapping() { address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); }
  ~Mapping() {
    if (address != MAP_FAILED) {
      munmap(address, size);
    }
  }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
};

bool IsMapped(void* address) {
  size_t length = 0;
  rex::memory::PageAccess access;
  return rex::memory::QueryProtect(address, length, access);
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

}  // namespace

REX_TEST_CASE("A fixed mapping that ended up at another address is removed and refused",
              "[base][memory]") {
  Mapping requested;
  Mapping stray;
  REQUIRE(requested.address != MAP_FAILED);
  REQUIRE(stray.address != MAP_FAILED);
  errno = 0;
  void* const accepted =
      rex::memory::detail::AcceptFixedMapping(requested.address, stray.size, stray.address);
  CHECK(accepted == MAP_FAILED);
  CHECK(errno == EEXIST);
  CHECK_FALSE(IsMapped(stray.address));
  stray.address = MAP_FAILED;
  CHECK(IsMapped(requested.address));
}

REX_TEST_CASE("A fixed mapping that ended up at the requested address is kept", "[base][memory]") {
  Mapping mapping;
  REQUIRE(mapping.address != MAP_FAILED);
  CHECK(rex::memory::detail::AcceptFixedMapping(mapping.address, mapping.size, mapping.address) ==
        mapping.address);
  CHECK(IsMapped(mapping.address));
}

REX_TEST_CASE("A mapping without a requested address is kept wherever it ended up",
              "[base][memory]") {
  Mapping mapping;
  REQUIRE(mapping.address != MAP_FAILED);
  CHECK(rex::memory::detail::AcceptFixedMapping(nullptr, mapping.size, mapping.address) ==
        mapping.address);
  CHECK(IsMapped(mapping.address));
}

REX_TEST_CASE("A failed mapping keeps its error", "[base][memory]") {
  Mapping requested;
  REQUIRE(requested.address != MAP_FAILED);
  errno = ENOMEM;
  CHECK(rex::memory::detail::AcceptFixedMapping(requested.address, requested.size, MAP_FAILED) ==
        MAP_FAILED);
  CHECK(errno == ENOMEM);
}

REX_TEST_CASE("Committing single pages of a reserved range reads no process files",
              "[base][memory]") {
  constexpr size_t kPages = 200;
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (!ReadSyscallCount()) {
    SKIP("the process I/O counters cannot be read");
  }
  void* const base =
      rex::memory::AllocFixed(nullptr, kPages * page, rex::memory::AllocationType::kReserve,
                              rex::memory::PageAccess::kNoAccess);
  REQUIRE(base != nullptr);
  const auto before = ReadSyscallCount();
  size_t committed = 0;
  for (size_t i = 0; i < kPages; ++i) {
    void* const address = static_cast<u8*>(base) + i * page;
    committed += rex::memory::AllocFixed(address, page, rex::memory::AllocationType::kCommit,
                                         rex::memory::PageAccess::kReadWrite) == address;
  }
  const auto after = ReadSyscallCount();
  CHECK(committed == kPages);
  REQUIRE((before && after));
  CHECK(*after - *before < 20);
  CHECK(rex::memory::DeallocFixed(base, kPages * page, rex::memory::DeallocationType::kRelease));
}

REX_TEST_CASE("A commit over a range with an unmapped page fails and leaves the mapped pages alone",
              "[base][memory]") {
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  u8* const base = static_cast<u8*>(rex::memory::AllocFixed(nullptr, 3 * page,
                                                            rex::memory::AllocationType::kReserve,
                                                            rex::memory::PageAccess::kNoAccess));
  REQUIRE(base != nullptr);
  REQUIRE(munmap(base + page, page) == 0);
  CHECK(rex::memory::AllocFixed(base, 3 * page, rex::memory::AllocationType::kCommit,
                                rex::memory::PageAccess::kReadWrite) == nullptr);
  for (u8* const address : {base, base + 2 * page}) {
    size_t length = 0;
    rex::memory::PageAccess access = rex::memory::PageAccess::kReadWrite;
    CHECK(rex::memory::QueryProtect(address, length, access));
    CHECK(access == rex::memory::PageAccess::kNoAccess);
  }
  CHECK(rex::memory::DeallocFixed(base, page, rex::memory::DeallocationType::kRelease));
  CHECK(rex::memory::DeallocFixed(base + 2 * page, page, rex::memory::DeallocationType::kRelease));
}

#endif
