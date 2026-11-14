/**
 * @file        tests/unit/kernel/shim_registry_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/shim.h>

using namespace rex::kernel::shim;

REX_TEST_CASE("Shim registry finds each entry by name", "[kernel][shim]") {
  for (const Entry& entry : kRegistry) {
    REQUIRE(Find(entry.name) == &entry);
    CHECK(IsRegistered(entry.name));
  }
}

REX_TEST_CASE("Shim registry rejects names that are not registered", "[kernel][shim]") {
  CHECK(Find("memcopy") == nullptr);
  CHECK_FALSE(IsRegistered(""));
  CHECK_FALSE(IsRegistered("rexshim_memcpy"));
  CHECK_FALSE(IsRegistered("GetDiskFreeSpaceExA"));
}

REX_TEST_CASE("Shim heap group is atomic and every heap entry requires the heap",
              "[kernel][shim]") {
  CHECK(TraitsOf(Group::kHeap).atomic);
  for (const Entry& entry : kRegistry) {
    if (entry.group == Group::kHeap) {
      CHECK(rex::HasFlags(entry.flags, Flag::kRequiresHeap));
    } else {
      CHECK_FALSE(rex::HasFlags(entry.flags, Flag::kRequiresHeap));
    }
  }
}
