/**
 * @file        tests/unit/core/fixed_mapping_test.cpp
 * @brief       The fixed address mapping check for kernels that take the no replace flag as a hint
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

#include <sys/mman.h>
#include <unistd.h>

#include <rex/memory/virtual_memory.h>

#include "core/memory_posix.h"

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

}  // namespace

REX_TEST_CASE("A fixed mapping that landed at another address is removed and refused",
              "[core][memory]") {
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

REX_TEST_CASE("A fixed mapping that landed at the requested address is kept", "[core][memory]") {
  Mapping mapping;
  REQUIRE(mapping.address != MAP_FAILED);
  CHECK(rex::memory::detail::AcceptFixedMapping(mapping.address, mapping.size, mapping.address) ==
        mapping.address);
  CHECK(IsMapped(mapping.address));
}

REX_TEST_CASE("A mapping without a requested address is kept wherever it landed",
              "[core][memory]") {
  Mapping mapping;
  REQUIRE(mapping.address != MAP_FAILED);
  CHECK(rex::memory::detail::AcceptFixedMapping(nullptr, mapping.size, mapping.address) ==
        mapping.address);
  CHECK(IsMapped(mapping.address));
}

REX_TEST_CASE("A failed mapping keeps its error", "[core][memory]") {
  Mapping requested;
  REQUIRE(requested.address != MAP_FAILED);
  errno = ENOMEM;
  CHECK(rex::memory::detail::AcceptFixedMapping(requested.address, requested.size, MAP_FAILED) ==
        MAP_FAILED);
  CHECK(errno == ENOMEM);
}

#endif
