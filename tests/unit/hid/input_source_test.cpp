/**
 * @file        tests/unit/hid/input_source_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <rex/input_keys.h>

using rex::InputSource;
using rex::InputSourceToString;
using rex::InputSourceType;
using rex::IsSameInput;
using rex::KeyModifiers;
using rex::PadAxes;
using rex::PadInput;
using rex::ParseInputSource;
using rex::VirtualKey;

namespace {

InputSource PadSource(PadInput button) {
  InputSource source;
  source.type = InputSourceType::kPadButton;
  source.code = static_cast<u16>(button);
  return source;
}

}  // namespace

REX_TEST_CASE("InputSource Every pad button has a distinct name that parses back",
              "[hid][input_source]") {
  std::set<std::string> names;
  for (u8 i = 0; i < rex::kPadButtonCount; ++i) {
    const InputSource source = PadSource(static_cast<PadInput>(i));
    const std::string text = InputSourceToString(source);
    REQUIRE_FALSE(text.empty());
    CHECK(names.insert(text).second);
    CHECK(rex::GetPadButtonName(static_cast<PadInput>(i)) == text);
    auto parsed = ParseInputSource(text);
    REQUIRE(parsed);
    CHECK(*parsed == source);
  }
  CHECK(rex::GetPadButtonName(static_cast<PadInput>(rex::kPadButtonCount)).empty());
}

REX_TEST_CASE("InputSource Pad names match the text saved configs use", "[hid][input_source]") {
  CHECK(InputSourceToString(PadSource(PadInput::kA)) == "PadA");
  CHECK(InputSourceToString(PadSource(PadInput::kDpadUp)) == "PadDUp");
  CHECK(InputSourceToString(PadSource(PadInput::kLeftStickPress)) == "PadLS");
  CHECK(InputSourceToString(PadSource(PadInput::kRightShoulder)) == "PadRB");
  CHECK(InputSourceToString(PadSource(PadInput::kLeftStickRight)) == "LStickRight");
  CHECK(InputSourceToString(PadSource(PadInput::kRightStickLeft)) == "RStickLeft");
}

REX_TEST_CASE("InputSource A whole stick parses and writes back", "[hid][input_source]") {
  for (const auto [text, axes] :
       {std::pair<std::string_view, PadAxes>{"LStick", PadAxes::kLeft},
        std::pair<std::string_view, PadAxes>{"RStick", PadAxes::kRight}}) {
    auto parsed = ParseInputSource(text);
    REQUIRE(parsed);
    CHECK(parsed->type == InputSourceType::kPadAxes);
    CHECK(parsed->code == static_cast<u16>(axes));
    CHECK(InputSourceToString(*parsed) == text);
  }
}

REX_TEST_CASE("InputSource Keys parse with their modifiers and write back", "[hid][input_source]") {
  auto plain = ParseInputSource("W");
  REQUIRE(plain);
  CHECK(plain->type == InputSourceType::kKey);
  CHECK(plain->code == static_cast<u16>(VirtualKey::kW));
  CHECK(plain->modifiers == KeyModifiers::kNone);
  CHECK(InputSourceToString(*plain) == "W");

  auto chord = ParseInputSource("Alt+Ctrl+Shift+K");
  REQUIRE(chord);
  CHECK(chord->code == static_cast<u16>(VirtualKey::kK));
  CHECK(chord->modifiers == (KeyModifiers::kShift | KeyModifiers::kCtrl | KeyModifiers::kAlt));
  CHECK(InputSourceToString(*chord) == "Shift+Ctrl+Alt+K");
  auto again = ParseInputSource(InputSourceToString(*chord));
  REQUIRE(again);
  CHECK(*again == *chord);
}

REX_TEST_CASE("InputSource Mouse buttons are keys with their virtual key", "[hid][input_source]") {
  auto left = ParseInputSource("LMB");
  REQUIRE(left);
  CHECK(left->type == InputSourceType::kKey);
  CHECK(left->code == static_cast<u16>(VirtualKey::kLButton));
  CHECK(InputSourceToString(*left) == "LMB");
  auto right = ParseInputSource("RMB");
  REQUIRE(right);
  CHECK(right->code == static_cast<u16>(VirtualKey::kRButton));
}

REX_TEST_CASE("InputSource The wheel parses to a direction and writes back",
              "[hid][input_source]") {
  auto up = ParseInputSource("WheelUp");
  REQUIRE(up);
  CHECK(up->type == InputSourceType::kMouseWheel);
  CHECK(up->sign == 1);
  CHECK(up->code == 0);
  CHECK(InputSourceToString(*up) == "WheelUp");

  auto down = ParseInputSource("Ctrl+WheelDown");
  REQUIRE(down);
  CHECK(down->type == InputSourceType::kMouseWheel);
  CHECK(down->sign == -1);
  CHECK(down->modifiers == KeyModifiers::kCtrl);
  CHECK(InputSourceToString(*down) == "Ctrl+WheelDown");
}

REX_TEST_CASE("InputSource Text that names no input is rejected as a config error",
              "[hid][input_source]") {
  for (const std::string_view text :
       {"", "Shift", "Ctrl+", "NoSuchKey", "Shift+PadA", "PadA+W", "padA", "lstick"}) {
    auto parsed = ParseInputSource(text);
    CHECK_FALSE(parsed);
    if (!parsed) {
      CHECK(parsed.error().category == rex::ErrorCategory::kConfig);
    }
  }
}

REX_TEST_CASE("InputSource A source that names nothing writes as empty text",
              "[hid][input_source]") {
  InputSource unnamed_key;
  unnamed_key.code = 0;
  CHECK(InputSourceToString(unnamed_key).empty());
  InputSource pad;
  pad.type = InputSourceType::kPadButton;
  pad.code = 200;
  CHECK(InputSourceToString(pad).empty());
  InputSource axes;
  axes.type = InputSourceType::kPadAxes;
  axes.code = static_cast<u16>(PadAxes::kNone);
  CHECK(InputSourceToString(axes).empty());
}

REX_TEST_CASE("InputSource IsSameInput ignores the stick row slot and equality does not",
              "[hid][input_source]") {
  auto first = ParseInputSource("W");
  auto second = ParseInputSource("W");
  REQUIRE(first);
  REQUIRE(second);
  second->axis_slot = 3;
  CHECK(IsSameInput(*first, *second));
  CHECK_FALSE(*first == *second);
  auto shifted = ParseInputSource("Shift+W");
  REQUIRE(shifted);
  CHECK_FALSE(IsSameInput(*first, *shifted));
}
