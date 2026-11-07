/**
 * @file        tests/unit/kernel/title_interfaces_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>

#include <rex/kernel/module.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>
#include <rex/module.h>

#include "kernel/kernel_fixture.h"
#include "kernel/title_state.h"
#include "xam/xam_state.h"

using rex::IInterface;
using rex::InterfaceType;
using rex::kernel::StateOf;
using rex::kernel::TitleRuntime;
using rex::kernel::TitleState;

namespace {

constexpr std::array<InterfaceType, 5> kTypes = {
    rex::kernel::IObjectTable::kInterface, rex::kernel::IThreadRegistry::kInterface,
    rex::kernel::IModuleRegistry::kInterface, rex::kernel::INotificationBroadcaster::kInterface,
    rex::kernel::IOverlappedDispatcher::kInterface};

using Found = std::array<IInterface*, 5>;

Found FindServed() {
  Found found{};
  for (size_t i = 0; i < kTypes.size(); ++i) {
    found[i] = TitleState::FindServedInterface(kTypes[i].name, kTypes[i].version);
  }
  return found;
}

Found FindThroughApp() {
  return {REX_INTERFACE(rex::kernel::IObjectTable), REX_INTERFACE(rex::kernel::IThreadRegistry),
          REX_INTERFACE(rex::kernel::IModuleRegistry),
          REX_INTERFACE(rex::kernel::INotificationBroadcaster),
          REX_INTERFACE(rex::kernel::IOverlappedDispatcher)};
}

Found Expected(TitleRuntime& runtime) {
  return {&runtime.GetObjectTable(), &runtime.GetThreadRegistry(), &runtime.GetModuleRegistry(),
          &runtime.GetNotificationBroadcaster(), &StateOf(runtime).GetOverlappedDispatcher()};
}

/** Looks the five up while the object table destroys it, which is inside the detach window. */
class ProbeObject final : public rex::kernel::IXAMObject {
 public:
  static constexpr rex::u32 kTag = 0x50524F42;

  explicit ProbeObject(Found& seen) : seen_(seen) {}
  ~ProbeObject() override { seen_ = FindServed(); }

  [[nodiscard]] rex::u32 GetTag() const override { return kTag; }

 private:
  Found& seen_;
};

}  // namespace

REX_TEST_CASE("a loaded title serves the five interfaces through the app factory",
              "[kernel][interfaces]") {
  rex::test::KernelFixture fixture;
  const Found expected = Expected(fixture.GetRuntime());
  const Found found = FindThroughApp();
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO(kTypes[i].name);
    CHECK(found[i] != nullptr);
    CHECK(found[i] == expected[i]);
  }
}

REX_TEST_CASE("no title serves none of the five interfaces", "[kernel][interfaces]") {
  const Found found = FindServed();
  for (size_t i = 0; i < found.size(); ++i) {
    INFO(kTypes[i].name);
    CHECK(found[i] == nullptr);
  }
  { rex::test::KernelFixture fixture; }
  const Found after = FindServed();
  for (size_t i = 0; i < after.size(); ++i) {
    INFO(kTypes[i].name);
    CHECK(after[i] == nullptr);
  }
}

REX_TEST_CASE("a second title is served after the first unloads", "[kernel][interfaces]") {
  { rex::test::KernelFixture first; }
  rex::test::KernelFixture second;
  const Found expected = Expected(second.GetRuntime());
  const Found found = FindThroughApp();
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO(kTypes[i].name);
    CHECK(found[i] == expected[i]);
  }
}

REX_TEST_CASE("XAM takes the interfaces of its title at attach", "[kernel][interfaces][xam]") {
  rex::test::KernelFixture fixture;
  TitleRuntime& runtime = fixture.GetRuntime();
  const auto& title = rex::xam::CurrentTitle();
  CHECK(&title.objects == &runtime.GetObjectTable());
  CHECK(&title.threads == &runtime.GetThreadRegistry());
  CHECK(&title.modules == &runtime.GetModuleRegistry());
  CHECK(&title.notifier == &runtime.GetNotificationBroadcaster());
  CHECK(&title.overlapped_dispatcher == &StateOf(runtime).GetOverlappedDispatcher());
}

REX_TEST_CASE("the interfaces are still served while the title tears down",
              "[kernel][interfaces]") {
  Found seen{};
  Found expected{};
  {
    rex::test::KernelFixture fixture;
    expected = Expected(fixture.GetRuntime());
    auto added =
        fixture.GetRuntime().GetObjectTable().Emplace<ProbeObject>(rex::kernel::AddOptions{}, seen);
    REQUIRE(added.has_value());
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO(kTypes[i].name);
    CHECK(seen[i] != nullptr);
    CHECK(seen[i] == expected[i]);
  }
}
