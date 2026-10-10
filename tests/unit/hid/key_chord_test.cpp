/**
 * @file        tests/unit/hid/key_chord_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hid/key_chord.h>

#include "hid/common/cvar_table.h"

using rex::hid::KeyChord;
using rex::hid::KeyChordToString;
using rex::hid::KeyModifiers;
using rex::hid::ParseKeyChord;
using rex::hid::ParseKeyChordList;
using rex::hid::VirtualKey;

REX_TEST_CASE("A plain key name parses to a chord with no modifiers", "[key_chord]") {
  auto chord = ParseKeyChord("F3");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{VirtualKey::kF3, KeyModifiers::None});
}

REX_TEST_CASE("Modifier prefixes parse in every order", "[key_chord]") {
  constexpr std::array<std::string_view, 8> kTexts = {
      "Shift+Ctrl+Alt+K", "Shift+Alt+Ctrl+K", "Ctrl+Shift+Alt+K",    "Ctrl+Alt+Shift+K",
      "Alt+Shift+Ctrl+K", "Alt+Ctrl+Shift+K", "Control+Alt+Shift+K", "Alt+Control+Shift+K"};
  for (const auto text : kTexts) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE(chord);
    CHECK(*chord ==
          KeyChord{VirtualKey::kK, KeyModifiers::Shift | KeyModifiers::Ctrl | KeyModifiers::Alt});
  }
}

REX_TEST_CASE("Control is the same modifier as Ctrl and formats as Ctrl", "[key_chord]") {
  auto long_form = ParseKeyChord("Control+K");
  auto short_form = ParseKeyChord("Ctrl+K");
  REQUIRE(long_form);
  REQUIRE(short_form);
  CHECK(*long_form == *short_form);
  CHECK(KeyChordToString(*long_form) == "Ctrl+K");
}

REX_TEST_CASE("Mouse button names parse with and without modifiers", "[key_chord]") {
  CHECK(*ParseKeyChord("LMB") == KeyChord{VirtualKey::kLButton});
  CHECK(*ParseKeyChord("RMB") == KeyChord{VirtualKey::kRButton});
  CHECK(*ParseKeyChord("MMB") == KeyChord{VirtualKey::kMButton});
  CHECK(*ParseKeyChord("Shift+LMB") == KeyChord{VirtualKey::kLButton, KeyModifiers::Shift});
}

REX_TEST_CASE("Wheel names parse with modifiers and round trip", "[key_chord]") {
  CHECK(*ParseKeyChord("WheelUp") == KeyChord{VirtualKey::kWheelUp});
  CHECK(*ParseKeyChord("WheelDown") == KeyChord{VirtualKey::kWheelDown});
  auto chord = ParseKeyChord("Ctrl+WheelUp");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{VirtualKey::kWheelUp, KeyModifiers::Ctrl});
  CHECK(KeyChordToString(*chord) == "Ctrl+WheelUp");
  CHECK(KeyChordToString({VirtualKey::kWheelDown, KeyModifiers::Shift}) == "Shift+WheelDown");
}

REX_TEST_CASE("An empty string is the unbound chord", "[key_chord]") {
  auto chord = ParseKeyChord("");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{});
  CHECK(KeyChordToString(KeyChord{}).empty());
}

REX_TEST_CASE("NumpadEnter is the Return key and not an unknown key", "[key_chord]") {
  auto chord = ParseKeyChord("NumpadEnter");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{VirtualKey::kReturn});
  CHECK(KeyChordToString(*chord) == "Return");
}

REX_TEST_CASE("Unknown names and malformed shapes are config errors naming the text",
              "[key_chord]") {
  constexpr std::array<std::string_view, 8> kBad = {
      "NoSuchKey", "f3", "Shift+NoSuchKey", "Shift+", "K+", "+", "Shift++K", "Ctrl+Alt+"};
  for (const auto text : kBad) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE_FALSE(chord);
    CHECK(chord.error().category == rex::ErrorCategory::Config);
    CHECK(chord.error().message.find(text) != std::string::npos);
  }
}

REX_TEST_CASE("A bare modifier is an error", "[key_chord]") {
  constexpr std::array<std::string_view, 6> kBare = {"Shift", "Ctrl",        "Control",
                                                     "Alt",   "Shift+Shift", "Ctrl+Alt"};
  for (const auto text : kBare) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE_FALSE(chord);
    CHECK(chord.error().category == rex::ErrorCategory::Config);
  }
}

REX_TEST_CASE("Chords format with modifiers in the order Shift, Ctrl, Alt", "[key_chord]") {
  const KeyModifiers all = KeyModifiers::Alt | KeyModifiers::Ctrl | KeyModifiers::Shift;
  CHECK(KeyChordToString({VirtualKey::kK, all}) == "Shift+Ctrl+Alt+K");
  CHECK(KeyChordToString({VirtualKey::kK, KeyModifiers::Alt | KeyModifiers::Shift}) ==
        "Shift+Alt+K");
  CHECK(KeyChordToString({VirtualKey::kF4, KeyModifiers::Alt}) == "Alt+F4");
}

REX_TEST_CASE("Chord text round trips", "[key_chord]") {
  constexpr std::array<std::string_view, 9> kTexts = {
      "F3",  "Backtick",         "Shift+Up", "Ctrl+K",  "Alt+F4",
      "LMB", "Shift+Ctrl+Alt+Z", "Ctrl+MMB", "PageDown"};
  for (const auto text : kTexts) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE(chord);
    CHECK(KeyChordToString(*chord) == text);
  }
}

REX_TEST_CASE("A chord whose key has no name formats as empty", "[key_chord]") {
  CHECK(KeyChordToString({VirtualKey::kXInputPadA, KeyModifiers::Shift}).empty());
  CHECK(KeyChordToString({VirtualKey::kNone, KeyModifiers::Ctrl}).empty());
}

REX_TEST_CASE("Lists split on commas and trim spaces and tabs", "[key_chord]") {
  auto list = ParseKeyChordList(" Shift+W , A\t,,Ctrl+X ,");
  REQUIRE(list);
  REQUIRE(list->size() == 3);
  CHECK((*list)[0] == KeyChord{VirtualKey::kW, KeyModifiers::Shift});
  CHECK((*list)[1] == KeyChord{VirtualKey::kA});
  CHECK((*list)[2] == KeyChord{VirtualKey::kX, KeyModifiers::Ctrl});

  auto pair = ParseKeyChordList("Q,I");
  REQUIRE(pair);
  CHECK(*pair == std::vector<KeyChord>{{VirtualKey::kQ}, {VirtualKey::kI}});

  auto empty = ParseKeyChordList("");
  REQUIRE(empty);
  CHECK(empty->empty());
  auto blanks = ParseKeyChordList(" , \t,");
  REQUIRE(blanks);
  CHECK(blanks->empty());
}

REX_TEST_CASE("A bad list item fails the whole list", "[key_chord]") {
  auto list = ParseKeyChordList("A,NoSuchKey,B");
  REQUIRE_FALSE(list);
  CHECK(list.error().category == rex::ErrorCategory::Config);
  CHECK(list.error().message.find("NoSuchKey") != std::string::npos);
  CHECK_FALSE(ParseKeyChordList("A,Shift"));
}

REX_TEST_CASE("Every hid.keybind default value parses", "[key_chord]") {
  rex::u32 checked = 0;
  for (const rex::cvar::IVar* var : rex::hid::cvars::kHIDCvars) {
    if (!var->desc().name.starts_with("hid.keybind_")) {
      continue;
    }
    INFO(var->desc().name);
    CHECK(ParseKeyChordList(var->DefaultText()));
    ++checked;
  }
  CHECK(checked == 25);

  auto a = ParseKeyChordList(rex::hid::cvars::keybind_a.DefaultText());
  REQUIRE(a);
  CHECK(*a == std::vector<KeyChord>{{VirtualKey::kOem1}, {VirtualKey::kSpace}});
  auto dpad_up = ParseKeyChordList(rex::hid::cvars::keybind_dpad_up.DefaultText());
  REQUIRE(dpad_up);
  CHECK(*dpad_up == std::vector<KeyChord>{{VirtualKey::kUp, KeyModifiers::Shift}});
  auto guide = ParseKeyChordList(rex::hid::cvars::keybind_guide.DefaultText());
  REQUIRE(guide);
  CHECK(guide->empty());
}

REX_TEST_CASE("The standard bind defaults parse to the overlay keys", "[key_chord]") {
  CHECK(*ParseKeyChord("F3") == KeyChord{VirtualKey::kF3});
  CHECK(*ParseKeyChord("Backtick") == KeyChord{VirtualKey::kOem3});
  CHECK(*ParseKeyChord("F4") == KeyChord{VirtualKey::kF4});
  CHECK(*ParseKeyChord("F7") == KeyChord{VirtualKey::kF7});
}
