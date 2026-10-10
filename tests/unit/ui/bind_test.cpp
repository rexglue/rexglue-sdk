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

#include <rex/hid/key_chord.h>
#include <rex/ui/ui_event.h>

#include "core/cvar_registry.h"
#include "ui/sdl3/bind_table.h"

using rex::hid::KeyChord;
using rex::hid::KeyModifiers;
using rex::hid::VirtualKey;
using rex::ui::BindTable;
using rex::ui::KeyEvent;
using rex::ui::UIEvent;
using rex::operator|;

namespace {

KeyEvent Down(VirtualKey key, KeyModifiers mods = KeyModifiers::None) {
  return KeyEvent(UIEvent::Type::KeyDown, nullptr, key, 1, false,
                  rex::HasFlags(mods, KeyModifiers::Shift), rex::HasFlags(mods, KeyModifiers::Ctrl),
                  rex::HasFlags(mods, KeyModifiers::Alt));
}

struct Harness {
  Harness() : registry(rex::detail::CreateCvarRegistry()) {
    REQUIRE(registry->Register("bind", {}));
    table.Attach(registry.get());
  }

  std::unique_ptr<rex::detail::CvarRegistryImpl> registry;
  BindTable table;
  rex::i32 fired = 0;
};

}  // namespace

REX_TEST_CASE("A bind writes its cvar from the default chord", "[bind]") {
  Harness h;
  h.table.Register("bind_console", {VirtualKey::kOem3}, "Toggle the console", [&h] { ++h.fired; });
  CHECK(h.registry->Get("bind.console").value() == "Backtick");
  CHECK(h.table.Process(Down(VirtualKey::kOem3)));
  CHECK(h.fired == 1);
  h.table.Register("settings", {VirtualKey::kF4, KeyModifiers::Alt}, "Toggle settings",
                   [&h] { ++h.fired; });
  CHECK(h.registry->Get("bind.settings").value() == "Alt+F4");
}

REX_TEST_CASE("A bind cvar change moves the bind to the new chord", "[bind]") {
  Harness h;
  h.table.Register("test", {VirtualKey::kF9}, "Test bind", [&h] { ++h.fired; });
  REQUIRE(h.registry->Set("bind.test", "Ctrl+K"));
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kF9)));
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kK)));
  CHECK(h.table.Process(Down(VirtualKey::kK, KeyModifiers::Ctrl)));
  CHECK(h.fired == 1);
  REQUIRE(h.registry->ResetToDefault("bind.test"));
  CHECK(h.table.Process(Down(VirtualKey::kF9)));
  CHECK(h.fired == 2);
}

REX_TEST_CASE("A chord with Ctrl fires only with Ctrl held", "[bind]") {
  Harness h;
  h.table.Register("test", {VirtualKey::kK, KeyModifiers::Ctrl}, "Test bind", [&h] { ++h.fired; });
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kK)));
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kK, KeyModifiers::Shift)));
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kK, KeyModifiers::Ctrl | KeyModifiers::Shift)));
  CHECK(h.fired == 0);
  CHECK(h.table.Process(Down(VirtualKey::kK, KeyModifiers::Ctrl)));
  CHECK(h.fired == 1);
}

REX_TEST_CASE("A bad bind value keeps the previous chord", "[bind]") {
  Harness h;
  h.table.Register("test", {VirtualKey::kF9}, "Test bind", [&h] { ++h.fired; });
  REQUIRE(h.registry->Set("bind.test", "Ctrl+NoSuchKey"));
  CHECK(h.registry->Get("bind.test").value() == "Ctrl+NoSuchKey");
  CHECK(h.table.Process(Down(VirtualKey::kF9)));
  CHECK(h.fired == 1);
  REQUIRE(h.registry->Set("bind.test", "Shift"));
  CHECK(h.table.Process(Down(VirtualKey::kF9)));
  CHECK(h.fired == 2);
}

REX_TEST_CASE("A bind with no default key is unbound until its cvar is set", "[bind]") {
  Harness h;
  h.table.Register("test", KeyChord{}, "Test bind", [&h] { ++h.fired; });
  CHECK(h.registry->Get("bind.test").value().empty());
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kNone)));
  REQUIRE(h.registry->Set("bind.test", "F8"));
  CHECK(h.table.Process(Down(VirtualKey::kF8)));
  CHECK(h.fired == 1);
}

REX_TEST_CASE("Unregister removes the cvar and the bind", "[bind]") {
  Harness h;
  h.table.Register("test", {VirtualKey::kF9}, "Test bind", [&h] { ++h.fired; });
  REQUIRE(h.registry->Find("bind.test"));
  h.table.Unregister("test");
  CHECK_FALSE(h.registry->Find("bind.test"));
  CHECK_FALSE(h.table.Process(Down(VirtualKey::kF9)));
  CHECK(h.fired == 0);
}

REX_TEST_CASE("A bind registered before Attach reaches the registry on Attach", "[bind]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("bind", {}));
  rex::i32 fired = 0;
  BindTable table;
  table.Register("test", {VirtualKey::kF9}, "Test bind", [&fired] { ++fired; });
  CHECK_FALSE(registry->Find("bind.test"));
  table.Attach(registry.get());
  CHECK(registry->Find("bind.test"));
  CHECK(table.Process(Down(VirtualKey::kF9)));
  CHECK(fired == 1);
  table.Attach(nullptr);
  CHECK_FALSE(registry->Find("bind.test"));
}
