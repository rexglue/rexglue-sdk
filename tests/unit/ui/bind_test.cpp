/**
 * @file        tests/unit/ui/bind_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>

#include <rex/hid/input_keys.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "hid/input_harness.h"
#include "ui/overlay/bind_table.h"

using rex::hid::InputEvent;
using rex::hid::KeyChord;
using rex::hid::KeyModifiers;
using rex::hid::VirtualKey;
using rex::ui::BindTable;

namespace {

InputEvent Down(VirtualKey key, KeyModifiers mods = KeyModifiers::kNone) {
  return InputEvent{.type = InputEvent::Type::kKeyDown, .key = key, .modifiers = mods};
}

std::string Read(std::string_view name) {
  auto found = REX_SETTINGS().Find(name);
  REQUIRE(found);
  return (*found)->GetText();
}

struct Harness {
  explicit Harness(rex::hid::IInputSystem& input) {
    table.SetInput(&input);
    table.Attach(&REX_SETTINGS());
  }
  ~Harness() { table.Attach(nullptr); }

  BindTable table;
  rex::i32 fired = 0;
};

void RunHarness(const std::function<void(Harness&)>& body) {
  rex::test_input::Run("none", [&body](rex::hid::IInputSystem& input) {
    Harness h(input);
    body(h);
  });
}

}  // namespace

REX_TEST_CASE("A bind writes its setting from the default chord", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("bind_console", {VirtualKey::kOem3}, "Toggle the console",
                     [&h] { ++h.fired; });
    CHECK(Read("ui.bind.console") == "Backtick");
    CHECK(h.table.Process(Down(VirtualKey::kOem3)));
    CHECK(h.fired == 1);
    h.table.Register("settings", {VirtualKey::kF4, KeyModifiers::kAlt}, "Toggle settings",
                     [&h] { ++h.fired; });
    CHECK(Read("ui.bind.settings") == "Alt+F4");
  });
}

REX_TEST_CASE("A bind setting change moves the bind to the new chord", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("test", {VirtualKey::kF9}, "Test bind", [&h] { ++h.fired; });
    REQUIRE(REX_SETTINGS().SetText("ui.bind.test", "Ctrl+K"));
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kF9)));
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kK)));
    CHECK(h.table.Process(Down(VirtualKey::kK, KeyModifiers::kCtrl)));
    CHECK(h.fired == 1);
    REQUIRE(REX_SETTINGS().Reset("ui.bind.test"));
    CHECK(h.table.Process(Down(VirtualKey::kF9)));
    CHECK(h.fired == 2);
  });
}

REX_TEST_CASE("A chord with Ctrl fires only with Ctrl held", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("test", {VirtualKey::kK, KeyModifiers::kCtrl}, "Test bind",
                     [&h] { ++h.fired; });
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kK)));
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kK, KeyModifiers::kShift)));
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kK, KeyModifiers::kCtrl | KeyModifiers::kShift)));
    CHECK(h.fired == 0);
    CHECK(h.table.Process(Down(VirtualKey::kK, KeyModifiers::kCtrl)));
    CHECK(h.fired == 1);
  });
}

REX_TEST_CASE("A bad bind value keeps the previous chord", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("test", {VirtualKey::kF9}, "Test bind", [&h] { ++h.fired; });
    REQUIRE(REX_SETTINGS().SetText("ui.bind.test", "Ctrl+NoSuchKey"));
    CHECK(Read("ui.bind.test") == "Ctrl+NoSuchKey");
    CHECK(h.table.Process(Down(VirtualKey::kF9)));
    CHECK(h.fired == 1);
    REQUIRE(REX_SETTINGS().SetText("ui.bind.test", "Shift"));
    CHECK(h.table.Process(Down(VirtualKey::kF9)));
    CHECK(h.fired == 2);
  });
}

REX_TEST_CASE("A bind with no default key is unbound until its setting is set", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("test", KeyChord{}, "Test bind", [&h] { ++h.fired; });
    CHECK(Read("ui.bind.test").empty());
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kNone)));
    REQUIRE(REX_SETTINGS().SetText("ui.bind.test", "F8"));
    CHECK(h.table.Process(Down(VirtualKey::kF8)));
    CHECK(h.fired == 1);
  });
}

REX_TEST_CASE("Unregister removes the setting and the bind", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("test", {VirtualKey::kF9}, "Test bind", [&h] { ++h.fired; });
    REQUIRE(REX_SETTINGS().Find("ui.bind.test"));
    h.table.Unregister("test");
    CHECK_FALSE(REX_SETTINGS().Find("ui.bind.test"));
    CHECK_FALSE(h.table.Process(Down(VirtualKey::kF9)));
    CHECK(h.fired == 0);
  });
}

REX_TEST_CASE("A bind registered before Attach reaches the registry on Attach", "[ui][bind]") {
  rex::test_input::Run("none", [](rex::hid::IInputSystem& input) {
    rex::i32 fired = 0;
    BindTable table;
    table.SetInput(&input);
    table.Register("test", {VirtualKey::kF9}, "Test bind", [&fired] { ++fired; });
    CHECK_FALSE(REX_SETTINGS().Find("ui.bind.test"));
    table.Attach(&REX_SETTINGS());
    CHECK(REX_SETTINGS().Find("ui.bind.test"));
    CHECK(table.Process(Down(VirtualKey::kF9)));
    CHECK(fired == 1);
    table.Attach(nullptr);
    CHECK_FALSE(REX_SETTINGS().Find("ui.bind.test"));
  });
}

REX_TEST_CASE("A bind without an input system keeps its default chord and shows its text unparsed",
              "[ui][bind]") {
  rex::i32 fired = 0;
  BindTable table;
  table.Attach(&REX_SETTINGS());
  table.Register("test", {VirtualKey::kF9}, "Test bind", [&fired] { ++fired; });
  CHECK(Read("ui.bind.test").empty());
  REQUIRE(REX_SETTINGS().SetText("ui.bind.test", "Ctrl+K"));
  CHECK(Read("ui.bind.test") == "Ctrl+K");
  CHECK_FALSE(table.Process(Down(VirtualKey::kK, KeyModifiers::kCtrl)));
  CHECK(table.Process(Down(VirtualKey::kF9)));
  CHECK(fired == 1);
  table.Attach(nullptr);
}

REX_TEST_CASE("overlay hotkey is ui.bind.name and the old bind name reaches it", "[ui][bind]") {
  RunHarness([](Harness& h) {
    h.table.Register("console", {VirtualKey::kOem3}, "Toggle the console", [&h] { ++h.fired; });
    REQUIRE(REX_SETTINGS().Find("ui.bind.console"));
    REQUIRE(REX_SETTINGS().SetText(rex::SettingLayer::kFile, "bind.console", "F6"));
    CHECK(Read("ui.bind.console") == "F6");
    CHECK(h.table.Process(Down(VirtualKey::kF6)));
    CHECK(h.fired == 1);
  });
}
