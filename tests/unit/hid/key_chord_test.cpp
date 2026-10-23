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

#include "hid/cvar_table.h"

using rex::hid::KeyChord;
using rex::hid::KeyChordToString;
using rex::hid::KeyModifiers;
using rex::hid::ParseKeyChord;
using rex::hid::ParseKeyChordList;
using rex::hid::VirtualKey;

REX_TEST_CASE("A plain key name parses to a chord with no modifiers", "[hid][key_chord]") {
  auto chord = ParseKeyChord("F3");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{VirtualKey::kF3, KeyModifiers::kNone});
}

REX_TEST_CASE("Modifier prefixes parse in every order", "[hid][key_chord]") {
  constexpr std::array<std::string_view, 8> kTexts = {
      "Shift+Ctrl+Alt+K", "Shift+Alt+Ctrl+K", "Ctrl+Shift+Alt+K",    "Ctrl+Alt+Shift+K",
      "Alt+Shift+Ctrl+K", "Alt+Ctrl+Shift+K", "Control+Alt+Shift+K", "Alt+Control+Shift+K"};
  for (const auto text : kTexts) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE(chord);
    CHECK(*chord == KeyChord{VirtualKey::kK,
                             KeyModifiers::kShift | KeyModifiers::kCtrl | KeyModifiers::kAlt});
  }
}

REX_TEST_CASE("Control is the same modifier as Ctrl and formats as Ctrl", "[hid][key_chord]") {
  auto long_form = ParseKeyChord("Control+K");
  auto short_form = ParseKeyChord("Ctrl+K");
  REQUIRE(long_form);
  REQUIRE(short_form);
  CHECK(*long_form == *short_form);
  CHECK(KeyChordToString(*long_form) == "Ctrl+K");
}

REX_TEST_CASE("Mouse button names parse with and without modifiers", "[hid][key_chord]") {
  CHECK(*ParseKeyChord("LMB") == KeyChord{VirtualKey::kLButton});
  CHECK(*ParseKeyChord("RMB") == KeyChord{VirtualKey::kRButton});
  CHECK(*ParseKeyChord("MMB") == KeyChord{VirtualKey::kMButton});
  CHECK(*ParseKeyChord("Shift+LMB") == KeyChord{VirtualKey::kLButton, KeyModifiers::kShift});
  CHECK(*ParseKeyChord("Mouse4") == KeyChord{VirtualKey::kXButton1});
  CHECK(*ParseKeyChord("Mouse5") == KeyChord{VirtualKey::kXButton2});
  CHECK(*ParseKeyChord("Ctrl+Mouse4") == KeyChord{VirtualKey::kXButton1, KeyModifiers::kCtrl});
  CHECK(KeyChordToString(KeyChord{VirtualKey::kXButton2, KeyModifiers::kShift}) == "Shift+Mouse5");
}

REX_TEST_CASE("Wheel names parse with modifiers and round trip", "[hid][key_chord]") {
  CHECK(*ParseKeyChord("WheelUp") == KeyChord{VirtualKey::kWheelUp});
  CHECK(*ParseKeyChord("WheelDown") == KeyChord{VirtualKey::kWheelDown});
  auto chord = ParseKeyChord("Ctrl+WheelUp");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{VirtualKey::kWheelUp, KeyModifiers::kCtrl});
  CHECK(KeyChordToString(*chord) == "Ctrl+WheelUp");
  CHECK(KeyChordToString({VirtualKey::kWheelDown, KeyModifiers::kShift}) == "Shift+WheelDown");
}

REX_TEST_CASE("An empty string is the unbound chord", "[hid][key_chord]") {
  auto chord = ParseKeyChord("");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{});
  CHECK(KeyChordToString(KeyChord{}).empty());
}

REX_TEST_CASE("NumpadEnter is the Return key and not an unknown key", "[hid][key_chord]") {
  auto chord = ParseKeyChord("NumpadEnter");
  REQUIRE(chord);
  CHECK(*chord == KeyChord{VirtualKey::kReturn});
  CHECK(KeyChordToString(*chord) == "Return");
}

REX_TEST_CASE("Unknown names and malformed shapes are config errors naming the text",
              "[hid][key_chord]") {
  constexpr std::array<std::string_view, 8> kBad = {
      "NoSuchKey", "f3", "Shift+NoSuchKey", "Shift+", "K+", "+", "Shift++K", "Ctrl+Alt+"};
  for (const auto text : kBad) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE_FALSE(chord);
    CHECK(chord.error().category == rex::ErrorCategory::kConfig);
    CHECK(chord.error().message.find(text) != std::string::npos);
  }
}

REX_TEST_CASE("A bare modifier is an error", "[hid][key_chord]") {
  constexpr std::array<std::string_view, 6> kBare = {"Shift", "Ctrl",        "Control",
                                                     "Alt",   "Shift+Shift", "Ctrl+Alt"};
  for (const auto text : kBare) {
    INFO(text);
    auto chord = ParseKeyChord(text);
    REQUIRE_FALSE(chord);
    CHECK(chord.error().category == rex::ErrorCategory::kConfig);
  }
}

REX_TEST_CASE("Chords format with modifiers in the order Shift, Ctrl, Alt", "[hid][key_chord]") {
  const KeyModifiers all = KeyModifiers::kAlt | KeyModifiers::kCtrl | KeyModifiers::kShift;
  CHECK(KeyChordToString({VirtualKey::kK, all}) == "Shift+Ctrl+Alt+K");
  CHECK(KeyChordToString({VirtualKey::kK, KeyModifiers::kAlt | KeyModifiers::kShift}) ==
        "Shift+Alt+K");
  CHECK(KeyChordToString({VirtualKey::kF4, KeyModifiers::kAlt}) == "Alt+F4");
}

REX_TEST_CASE("Chord text round trips", "[hid][key_chord]") {
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

REX_TEST_CASE("A chord whose key has no name formats as empty", "[hid][key_chord]") {
  CHECK(KeyChordToString({VirtualKey::kXInputPadA, KeyModifiers::kShift}).empty());
  CHECK(KeyChordToString({VirtualKey::kNone, KeyModifiers::kCtrl}).empty());
}

REX_TEST_CASE("Lists split on commas and trim spaces and tabs", "[hid][key_chord]") {
  auto list = ParseKeyChordList(" Shift+W , A\t,,Ctrl+X ,");
  REQUIRE(list);
  REQUIRE(list->size() == 3);
  CHECK((*list)[0] == KeyChord{VirtualKey::kW, KeyModifiers::kShift});
  CHECK((*list)[1] == KeyChord{VirtualKey::kA});
  CHECK((*list)[2] == KeyChord{VirtualKey::kX, KeyModifiers::kCtrl});

  auto crlf = ParseKeyChordList("Shift+W,\r\nA\r\n");
  REQUIRE(crlf);
  CHECK(*crlf == std::vector<KeyChord>{{VirtualKey::kW, KeyModifiers::kShift}, {VirtualKey::kA}});

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

REX_TEST_CASE("A bad list item fails the whole list", "[hid][key_chord]") {
  auto list = ParseKeyChordList("A,NoSuchKey,B");
  REQUIRE_FALSE(list);
  CHECK(list.error().category == rex::ErrorCategory::kConfig);
  CHECK(list.error().message.find("NoSuchKey") != std::string::npos);
  CHECK_FALSE(ParseKeyChordList("A,Shift"));
}

REX_TEST_CASE("Every hid.keybind default value parses", "[hid][key_chord]") {
  rex::u32 checked = 0;
  for (const rex::cvar::VarBase* var : rex::hid::cvars::kHIDCvars) {
    if (!var->GetDesc().name.starts_with("hid.keybind_")) {
      continue;
    }
    INFO(var->GetDesc().name);
    CHECK(ParseKeyChordList(var->GetDefaultText()));
    ++checked;
  }
  CHECK(checked == 25);

  auto a = ParseKeyChordList(rex::hid::cvars::keybind_a.GetDefaultText());
  REQUIRE(a);
  CHECK(*a == std::vector<KeyChord>{{VirtualKey::kOem1}, {VirtualKey::kSpace}});
  auto dpad_up = ParseKeyChordList(rex::hid::cvars::keybind_dpad_up.GetDefaultText());
  REQUIRE(dpad_up);
  CHECK(*dpad_up == std::vector<KeyChord>{{VirtualKey::kUp, KeyModifiers::kShift}});
  auto guide = ParseKeyChordList(rex::hid::cvars::keybind_guide.GetDefaultText());
  REQUIRE(guide);
  CHECK(guide->empty());
}

REX_TEST_CASE("The standard bind defaults parse to the overlay keys", "[hid][key_chord]") {
  CHECK(*ParseKeyChord("F3") == KeyChord{VirtualKey::kF3});
  CHECK(*ParseKeyChord("Backtick") == KeyChord{VirtualKey::kOem3});
  CHECK(*ParseKeyChord("F4") == KeyChord{VirtualKey::kF4});
  CHECK(*ParseKeyChord("F7") == KeyChord{VirtualKey::kF7});
}
