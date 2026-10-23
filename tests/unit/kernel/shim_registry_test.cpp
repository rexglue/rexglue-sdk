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

#include <algorithm>

#include <rex/kernel/shim/registry.h>

namespace {

using namespace rex::kernel::shim;

size_t CountInGroup(Group group) {
  return static_cast<size_t>(
      std::count_if(kRegistry.begin(), kRegistry.end(),
                    [group](const Entry& entry) { return entry.group == group; }));
}

}  // namespace

REX_TEST_CASE("Shim registry holds every hook name", "[kernel][shim]") {
  CHECK(kRegistry.size() == 69);
  CHECK(CountInGroup(Group::kFile) == 24);
  CHECK(CountInGroup(Group::kHeap) == 4);
  CHECK(CountInGroup(Group::kMemory) == 10);
  CHECK(CountInGroup(Group::kString) == 23);
  CHECK(CountInGroup(Group::kMath) == 2);
  CHECK(CountInGroup(Group::kDebug) == 1);
  CHECK(CountInGroup(Group::kThreading) == 5);
}

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

REX_TEST_CASE("Shim group traits round trip", "[kernel][shim]") {
  for (const GroupTraits& traits : kGroups) {
    CHECK(TraitsOf(traits.id).id == traits.id);
    CHECK(CountInGroup(traits.id) > 0);
  }
}
