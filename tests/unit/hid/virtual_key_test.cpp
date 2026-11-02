/**
 * @file        tests/unit/hid/virtual_key_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string_view>

#include <rex/hid/input_keys.h>

using rex::hid::ParseVirtualKey;
using rex::hid::VirtualKey;
using rex::hid::VirtualKeyToString;

REX_TEST_CASE("Enter is named Return and NumpadEnter parses to it", "[hid]") {
  CHECK(VirtualKeyToString(VirtualKey::kReturn) == "Return");
  CHECK(ParseVirtualKey("NumpadEnter") == VirtualKey::kReturn);
  CHECK(ParseVirtualKey("Return") == VirtualKey::kReturn);
}

REX_TEST_CASE("Key names round trip", "[hid]") {
  constexpr std::array<std::string_view, 10> kNames = {
      "F1", "A", "0", "Backtick", "Escape", "PageUp", "Numpad5", "NumpadPlus", "CapsLock", "LMB"};
  for (const auto name : kNames) {
    INFO(name);
    CHECK(VirtualKeyToString(ParseVirtualKey(name)) == name);
  }
}

REX_TEST_CASE("The extra mouse buttons are named Mouse4 and Mouse5", "[hid]") {
  CHECK(VirtualKeyToString(VirtualKey::kXButton1) == "Mouse4");
  CHECK(VirtualKeyToString(VirtualKey::kXButton2) == "Mouse5");
  CHECK(ParseVirtualKey("Mouse4") == VirtualKey::kXButton1);
  CHECK(ParseVirtualKey("Mouse5") == VirtualKey::kXButton2);
  CHECK(ParseVirtualKey("Mouse6") == VirtualKey::kNone);
}

REX_TEST_CASE("Unknown names and keys map to nothing", "[hid]") {
  CHECK(ParseVirtualKey("") == VirtualKey::kNone);
  CHECK(ParseVirtualKey("NoSuchKey") == VirtualKey::kNone);
  CHECK(ParseVirtualKey("return") == VirtualKey::kNone);
  CHECK(VirtualKeyToString(VirtualKey::kNone).empty());
}
