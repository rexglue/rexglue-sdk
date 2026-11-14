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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rex/hid/binding.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "base/app_harness.h"
#include "hid/binding_set.h"

using rex::InputSource;
using rex::InputSourceType;
using rex::PadAxes;
using rex::ParseInputSource;
using rex::SettingLayer;
using rex::hid::BindingConfig;
using rex::hid::BindingSet;

namespace {

constexpr u32 kGameplay = 1;
constexpr u32 kMenus = 2;

constexpr size_t kJump = 0;
constexpr size_t kFire = 1;
constexpr size_t kMenu = 2;
constexpr size_t kMove = 3;

constexpr std::array<BindingConfig, 4> kDescs = {{
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
  Registered() : set(kDescs) { REQUIRE(set.Register(REX_SETTINGS())); }

  std::string Text(std::string_view name) const {
    auto found = REX_SETTINGS().Find(name);
    REQUIRE(found);
    return (*found)->GetText();
  }

  void Write(std::string_view name, std::string_view text) {
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kTitle, name, text));
  }

  BindingSet set;
};

void Run(const std::function<void(Registered&)>& body) {
  rex::test::RunInInit([&body] {
    Registered fixture;
    body(fixture);
  });
}

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
  CHECK(move[4].type == InputSourceType::kPadAxes);
  CHECK(move[4].axis_slot == 0);
  CHECK(set.TakeParseErrors().empty());
}

REX_TEST_CASE("BindingSet follows the setting when something else sets it", "[hid][bindings]") {
  Run([](Registered& fixture) {
    const u32 before = fixture.set.GetGeneration();
    fixture.Write("bind.jump", "Q, Shift+E");
    const auto jump = fixture.set.GetSources(kJump);
    REQUIRE(jump.size() == 2);
    CHECK(jump[0] == Parse("Q"));
    CHECK(jump[1] == Parse("Shift+E"));
    CHECK(fixture.set.GetGeneration() > before);
  });
}

REX_TEST_CASE("BindingSet reads a value a file carried before it registered", "[hid][bindings]") {
  rex::test::RunInInit([] {
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "bind.jump", "Q,Shift+E"));
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kCommandLine, "bind.move", "Up,Down,Left,Right"));
    Registered fixture;
    const auto jump = fixture.set.GetSources(kJump);
    REQUIRE(jump.size() == 2);
    CHECK(jump[0] == Parse("Q"));
    CHECK(jump[1] == Parse("Shift+E"));
    CHECK(fixture.set.GetSources(kMove).size() == 4);
    CHECK(fixture.Text("bind.jump") == "Q,Shift+E");
  });
}

REX_TEST_CASE("BindingSet settings are key chord lists in their own scope", "[hid][bindings]") {
  Run([](Registered&) {
    for (const auto& config : kDescs) {
      auto found = REX_SETTINGS().Find(std::string("bind.") + std::string(config.name));
      REQUIRE(found);
      CHECK((*found)->GetConstraints().key_chord);
      CHECK((*found)->GetDefaultText() == config.defaults);
    }
    CHECK(REX_SETTINGS().Enumerate("bind").size() == kDescs.size());
  });
}

REX_TEST_CASE("BindingSet SetSource replaces a slot and appends past the end", "[hid][bindings]") {
  Run([](Registered& fixture) {
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
  });
}

REX_TEST_CASE("BindingSet SetSources and Clear write the setting text", "[hid][bindings]") {
  Run([](Registered& fixture) {
    const std::array<InputSource, 2> list = {Parse("PadX"), Parse("Ctrl+K")};
    CHECK(fixture.set.SetSources(kFire, list));
    CHECK(fixture.Text("bind.fire") == "PadX,Ctrl+K");
    CHECK(fixture.set.Clear(kFire));
    CHECK(fixture.Text("bind.fire").empty());
    CHECK(fixture.set.GetSources(kFire).empty());
  });
}

REX_TEST_CASE("BindingSet Reset and ResetGroup restore the defaults", "[hid][bindings]") {
  Run([](Registered& fixture) {
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
  });
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
  Run([](Registered& fixture) {
    fixture.Write("bind.jump", "W,W,Shift+W,W");
    const auto jump = fixture.set.GetSources(kJump);
    REQUIRE(jump.size() == 2);
    CHECK(jump[0] == Parse("W"));
    CHECK(jump[1] == Parse("Shift+W"));
  });
}

REX_TEST_CASE("BindingSet stick rows are positional and keep empty slots", "[hid][bindings]") {
  Run([](Registered& fixture) {
    fixture.Write("bind.move", "W,,A,D");
    const auto move = fixture.set.GetSources(kMove);
    REQUIRE(move.size() == 3);
    CHECK(move[0].axis_slot == 1);
    CHECK(move[1].axis_slot == 3);
    CHECK(move[2].axis_slot == 4);
    CHECK(fixture.set.SetSources(kMove, move));
    CHECK(fixture.Text("bind.move") == "W,,A,D");
  });
}

REX_TEST_CASE("BindingSet stick rows take a full row", "[hid][bindings]") {
  Run([](Registered& fixture) {
    fixture.Write("bind.move", "W,A,A,D,LStick,Up,Down,Left,Right");
    const auto full = fixture.set.GetSources(kMove);
    CHECK(full.size() == 9);
    CHECK(fixture.set.TakeParseErrors().empty());
  });
}

REX_TEST_CASE("BindingSet stick rows refuse an overlong row", "[hid][bindings]") {
  Run([](Registered& fixture) {
    fixture.Write("bind.move", "W,S,A,D,Q,E,Up,Down,Left,LStick");
    CHECK(fixture.set.GetSources(kMove).size() == 5);
    const auto errors = fixture.set.TakeParseErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("move") != std::string::npos);
    CHECK(errors[0].find("Left") != std::string::npos);
    CHECK(fixture.set.TakeParseErrors().empty());
  });
}

REX_TEST_CASE("BindingSet refuses text with a bad item and keeps the previous inputs",
              "[hid][bindings]") {
  Run([](Registered& fixture) {
    const u32 before = fixture.set.GetGeneration();
    fixture.Write("bind.jump", "Q,NoSuchKey,E");
    const auto jump = fixture.set.GetSources(kJump);
    REQUIRE(jump.size() == 2);
    CHECK(jump[0] == Parse("Space"));
    CHECK(jump[1] == Parse("PadA"));
    CHECK(fixture.set.GetGeneration() == before);
    const auto errors = fixture.set.TakeParseErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("jump") != std::string::npos);
    CHECK(errors[0].find("NoSuchKey") != std::string::npos);

    fixture.Write("bind.jump", "Q,E");
    REQUIRE(fixture.set.GetSources(kJump).size() == 2);
    CHECK(fixture.set.GetSources(kJump)[0] == Parse("Q"));
    CHECK(fixture.set.GetGeneration() > before);
    CHECK(fixture.set.TakeParseErrors().empty());
  });
}

REX_TEST_CASE("BindingSet Find maps a setting name to its binding", "[hid][bindings]") {
  BindingSet set(kDescs);
  CHECK(set.FindBySetting("bind.fire") == std::optional<size_t>(kFire));
  CHECK(set.FindBySetting("bind.move") == std::optional<size_t>(kMove));
  CHECK_FALSE(set.FindBySetting("fire"));
  CHECK_FALSE(set.FindBySetting("bindfire"));
  CHECK_FALSE(set.FindBySetting("bind.nosuch"));
  CHECK_FALSE(set.FindBySetting("other.fire"));
  CHECK_FALSE(set.FindBySetting("bind."));
}

REX_TEST_CASE("BindingSet refuses edits until it is registered and after it is not",
              "[hid][bindings]") {
  rex::test::RunInInit([] {
    BindingSet set(kDescs);
    CHECK_FALSE(set.SetSource(kJump, 0, Parse("F")));
    CHECK_FALSE(set.Clear(kJump));
    CHECK_FALSE(set.Reset(kJump));
    CHECK(set.GetSources(kJump).size() == 2);

    auto& settings = REX_SETTINGS();
    REQUIRE(set.Register(settings));
    CHECK_FALSE(set.Register(settings));
    CHECK(settings.Find("bind.jump"));
    CHECK(set.SetSource(kJump, 0, Parse("F")));
    set.Unregister();
    CHECK_FALSE(settings.Find("bind.jump"));
    CHECK_FALSE(set.SetSource(kJump, 0, Parse("G")));
    CHECK(set.GetSources(kJump)[0] == Parse("F"));
  });
}

REX_TEST_CASE("BindingSet answers an index out of range without failing", "[hid][bindings]") {
  Run([](Registered& fixture) {
    CHECK(fixture.set.GetSources(99).empty());
    CHECK_FALSE(fixture.set.SetSource(99, 0, Parse("F")));
    CHECK_FALSE(fixture.set.SetSources(99, {}));
    CHECK_FALSE(fixture.set.Clear(99));
    CHECK_FALSE(fixture.set.Reset(99));
  });
}
