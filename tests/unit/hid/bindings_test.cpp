/**
 * @file        tests/unit/hid/bindings_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rex/hid/bindings.h>

#include "core/cvar_registry.h"

using rex::hid::BindingDesc;
using rex::hid::BindingSet;
using rex::hid::InputSource;
using rex::hid::InputSourceKind;
using rex::hid::PadAxes;
using rex::hid::ParseInputSource;

namespace {

constexpr u32 kGameplay = 1;
constexpr u32 kMenus = 2;

constexpr size_t kJump = 0;
constexpr size_t kFire = 1;
constexpr size_t kMenu = 2;
constexpr size_t kMove = 3;

constexpr std::array<BindingDesc, 4> kDescs = {{
    {"jump", "Space,PadA", "Jump", kGameplay, PadAxes::kNone},
    {"fire", "LMB,PadRT", "Fire", kGameplay, PadAxes::kNone},
    {"menu", "Escape", "Open the menu", kMenus, PadAxes::kNone},
    {"move", "W,S,A,D,LStick", "Move", kGameplay, PadAxes::kLeft},
}};

InputSource Parse(std::string_view text) {
  auto parsed = ParseInputSource(text);
  REQUIRE(parsed);
  return *parsed;
}

struct Registered {
  Registered() : registry(rex::detail::CreateCvarRegistry()), set(kDescs) {
    REQUIRE(registry->Register("bind", {}));
    REQUIRE(set.Register(*registry));
  }

  std::string Text(std::string_view name) const {
    auto value = registry->Get(name);
    REQUIRE(value);
    return *value;
  }

  decltype(rex::detail::CreateCvarRegistry()) registry;
  BindingSet set;
};

}  // namespace

REX_TEST_CASE("BindingSet parses every default on construction", "[hid][bindings]") {
  BindingSet set(kDescs);
  REQUIRE(set.GetCount() == 4);
  const auto jump = set.GetSources(kJump);
  REQUIRE(jump.size() == 2);
  CHECK(jump[0] == Parse("Space"));
  CHECK(jump[1] == Parse("PadA"));
  const auto move = set.GetSources(kMove);
  REQUIRE(move.size() == 5);
  CHECK(move[0].axis_slot == 1);
  CHECK(move[3].axis_slot == 4);
  CHECK(move[4].kind == InputSourceKind::kPadAxes);
  CHECK(move[4].axis_slot == 0);
  CHECK(set.TakeParseErrors().empty());
}

REX_TEST_CASE("BindingSet follows the cvar when something else sets it", "[hid][bindings]") {
  Registered fixture;
  const u32 before = fixture.set.GetGeneration();
  REQUIRE(fixture.registry->Set("bind.jump", "Q, Shift+E"));
  const auto jump = fixture.set.GetSources(kJump);
  REQUIRE(jump.size() == 2);
  CHECK(jump[0] == Parse("Q"));
  CHECK(jump[1] == Parse("Shift+E"));
  CHECK(fixture.set.GetGeneration() > before);
}

REX_TEST_CASE("BindingSet SetSource replaces a slot and appends past the end", "[hid][bindings]") {
  Registered fixture;
  CHECK(fixture.set.SetSource(kJump, 0, Parse("F")));
  CHECK(fixture.Text("bind.jump") == "F,PadA");
  CHECK(fixture.set.SetSource(kJump, 9, Parse("Shift+Q")));
  CHECK(fixture.Text("bind.jump") == "F,PadA,Shift+Q");
  const auto jump = fixture.set.GetSources(kJump);
  REQUIRE(jump.size() == 3);
  CHECK(jump[2] == Parse("Shift+Q"));
  InputSource nothing;
  CHECK_FALSE(fixture.set.SetSource(kJump, 0, nothing));
  CHECK(fixture.set.GetSources(kJump).size() == 3);
}

REX_TEST_CASE("BindingSet SetSources and Clear write the cvar text", "[hid][bindings]") {
  Registered fixture;
  const std::array<InputSource, 2> list = {Parse("PadX"), Parse("Ctrl+K")};
  CHECK(fixture.set.SetSources(kFire, list));
  CHECK(fixture.Text("bind.fire") == "PadX,Ctrl+K");
  CHECK(fixture.set.Clear(kFire));
  CHECK(fixture.Text("bind.fire").empty());
  CHECK(fixture.set.GetSources(kFire).empty());
}

REX_TEST_CASE("BindingSet Reset and ResetGroup restore the defaults", "[hid][bindings]") {
  Registered fixture;
  REQUIRE(fixture.set.Clear(kJump));
  REQUIRE(fixture.set.Clear(kFire));
  REQUIRE(fixture.set.Clear(kMenu));
  CHECK(fixture.set.Reset(kJump));
  CHECK(fixture.set.GetSources(kJump).size() == 2);
  CHECK(fixture.set.GetSources(kFire).empty());
  CHECK(fixture.set.ResetGroup(kGameplay));
  CHECK(fixture.set.GetSources(kFire).size() == 2);
  CHECK(fixture.set.GetSources(kMenu).empty());
  CHECK(fixture.set.ResetGroup(kMenus));
  CHECK(fixture.set.GetSources(kMenu).size() == 1);
  CHECK_FALSE(fixture.set.ResetGroup(77));
}

REX_TEST_CASE("BindingSet FindConflict looks only inside the group", "[hid][bindings]") {
  BindingSet set(kDescs);
  CHECK(set.FindConflict(kFire, Parse("Space")) == std::optional<size_t>(kJump));
  CHECK_FALSE(set.FindConflict(kJump, Parse("Space")));
  CHECK_FALSE(set.FindConflict(kFire, Parse("Escape")));
  CHECK_FALSE(set.FindConflict(kFire, Parse("Shift+Space")));
  CHECK(set.FindConflict(kJump, Parse("W")) == std::optional<size_t>(kMove));
  CHECK_FALSE(set.FindConflict(99, Parse("Space")));
}

REX_TEST_CASE("BindingSet folds repeats in a plain list", "[hid][bindings]") {
  Registered fixture;
  REQUIRE(fixture.registry->Set("bind.jump", "W,W,Shift+W,W"));
  const auto jump = fixture.set.GetSources(kJump);
  REQUIRE(jump.size() == 2);
  CHECK(jump[0] == Parse("W"));
  CHECK(jump[1] == Parse("Shift+W"));
}

REX_TEST_CASE("BindingSet stick rows are positional and keep empty slots", "[hid][bindings]") {
  Registered fixture;
  REQUIRE(fixture.registry->Set("bind.move", "W,,A,D"));
  const auto move = fixture.set.GetSources(kMove);
  REQUIRE(move.size() == 3);
  CHECK(move[0].axis_slot == 1);
  CHECK(move[1].axis_slot == 3);
  CHECK(move[2].axis_slot == 4);
  CHECK(fixture.set.SetSources(kMove, move));
  CHECK(fixture.Text("bind.move") == "W,,A,D");
  REQUIRE(fixture.registry->Set("bind.move", "W,A,A,D,LStick,Up,Down,Left,Right"));
  const auto full = fixture.set.GetSources(kMove);
  CHECK(full.size() == 9);
  CHECK(fixture.set.TakeParseErrors().empty());
  REQUIRE(fixture.registry->Set("bind.move", "W,S,A,D,Q,E,Up,Down,Left,LStick"));
  CHECK(fixture.set.GetSources(kMove).size() == 9);
  const auto errors = fixture.set.TakeParseErrors();
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("move") != std::string::npos);
  CHECK(errors[0].find("Left") != std::string::npos);
  CHECK(fixture.set.TakeParseErrors().empty());
}

REX_TEST_CASE("BindingSet keeps the good items and reports the bad ones", "[hid][bindings]") {
  Registered fixture;
  REQUIRE(fixture.registry->Set("bind.jump", "Space,NoSuchKey,Q"));
  CHECK(fixture.set.GetSources(kJump).size() == 2);
  const auto errors = fixture.set.TakeParseErrors();
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("jump") != std::string::npos);
  CHECK(errors[0].find("NoSuchKey") != std::string::npos);
}

REX_TEST_CASE("BindingSet refuses edits until it is registered and after it is not",
              "[hid][bindings]") {
  BindingSet set(kDescs);
  CHECK_FALSE(set.SetSource(kJump, 0, Parse("F")));
  CHECK_FALSE(set.Clear(kJump));
  CHECK_FALSE(set.Reset(kJump));
  CHECK(set.GetSources(kJump).size() == 2);

  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("bind", {}));
  REQUIRE(set.Register(*registry));
  CHECK_FALSE(set.Register(*registry));
  CHECK(registry->Get("bind.jump"));
  CHECK(set.SetSource(kJump, 0, Parse("F")));
  set.Unregister();
  CHECK_FALSE(registry->Get("bind.jump"));
  CHECK_FALSE(set.SetSource(kJump, 0, Parse("G")));
  CHECK(set.GetSources(kJump)[0] == Parse("F"));
}

REX_TEST_CASE("BindingSet answers an index out of range without failing", "[hid][bindings]") {
  Registered fixture;
  CHECK(fixture.set.GetSources(99).empty());
  CHECK_FALSE(fixture.set.SetSource(99, 0, Parse("F")));
  CHECK_FALSE(fixture.set.SetSources(99, {}));
  CHECK_FALSE(fixture.set.Clear(99));
  CHECK_FALSE(fixture.set.Reset(99));
}
