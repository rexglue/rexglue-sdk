/**
 * @file        tests/unit/hid/null_backend_test.cpp
 * @brief       hid::NullHIDBackend reports 1 idle pad on user 0
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <vector>

#include <rex/hid.h>

using rex::hid::CodeOf;
using rex::hid::HIDError;
using rex::hid::NullHIDBackend;

REX_TEST_CASE("NullHIDBackend reports an idle pad on user 0 only", "[hid]") {
  NullHIDBackend backend;
  auto state = backend.GetState(0);
  REQUIRE(state);
  CHECK(static_cast<rex::u16>(state->gamepad.buttons) == 0);
  auto caps = backend.GetCapabilities(0, 0);
  REQUIRE(caps);
  CHECK(caps->type == rex::hid::XINPUT_DEVTYPE_GAMEPAD);
  for (rex::u32 user = 1; user < rex::hid::kMaxTitleUsers; ++user) {
    auto missing = backend.GetState(user);
    REQUIRE_FALSE(missing);
    CHECK(CodeOf(missing.error()) == HIDError::NotConnected);
  }
}

REX_TEST_CASE("NullHIDBackend has no keystrokes and accepts vibration", "[hid]") {
  NullHIDBackend backend;
  auto keystroke = backend.GetKeystroke(0, 0);
  REQUIRE_FALSE(keystroke);
  CHECK(CodeOf(keystroke.error()) == HIDError::Empty);
  CHECK(backend.SetVibration(0, {}));
  CHECK_FALSE(backend.SetVibration(1, {}));
}

REX_TEST_CASE("NullHIDBackend reports the idle pad connected once", "[hid]") {
  NullHIDBackend backend;
  std::vector<rex::u32> connected;
  backend.SetConnectionListener([&connected](rex::u32 user, bool on) {
    if (on) {
      connected.push_back(user);
    }
  });
  CHECK(backend.GetState(0));
  CHECK(backend.GetState(0));
  CHECK(connected == std::vector<rex::u32>{0});
}

REX_TEST_CASE("NullHIDBackend has no keyboard or pointer", "[hid]") {
  NullHIDBackend backend;
  backend.OnEvent({.kind = rex::hid::InputEvent::Kind::KeyDown, .key = rex::hid::VirtualKey::kA});
  CHECK_FALSE(backend.IsKeyDown(rex::hid::VirtualKey::kA));
  CHECK_FALSE(backend.pointer().has_position);
  CHECK(backend.pointer_mode() == rex::hid::PointerMode::Free);
}
