/**
 * @file        tests/unit/hid/null_backend_test.cpp
 * @brief       hid::NullBackend reports 1 idle pad on user 0
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <rex/hid.h>

using rex::hid::CodeOf;
using rex::hid::Error;
using rex::hid::NullBackend;

TEST_CASE("NullBackend reports an idle pad on user 0 only", "[hid]") {
  NullBackend backend;
  auto state = backend.GetState(0);
  REQUIRE(state);
  CHECK(static_cast<rex::u16>(state->gamepad.buttons) == 0);
  auto caps = backend.GetCapabilities(0, 0);
  REQUIRE(caps);
  CHECK(caps->type == rex::hid::XINPUT_DEVTYPE_GAMEPAD);
  for (rex::u32 user = 1; user < rex::hid::kMaxGuestUsers; ++user) {
    auto missing = backend.GetState(user);
    REQUIRE_FALSE(missing);
    CHECK(CodeOf(missing.error()) == Error::NotConnected);
  }
}

TEST_CASE("NullBackend has no keystrokes and accepts vibration", "[hid]") {
  NullBackend backend;
  auto keystroke = backend.GetKeystroke(0, 0);
  REQUIRE_FALSE(keystroke);
  CHECK(CodeOf(keystroke.error()) == Error::Empty);
  CHECK(backend.SetVibration(0, {}));
  CHECK_FALSE(backend.SetVibration(1, {}));
}

TEST_CASE("NullBackend reports the idle pad connected once", "[hid]") {
  NullBackend backend;
  std::vector<rex::u32> connected;
  backend.SetConnectionListener([&connected](rex::u32 user, bool on) {
    if (on) {
      connected.push_back(user);
    }
  });
  (void)backend.GetState(0);
  (void)backend.GetState(0);
  CHECK(connected == std::vector<rex::u32>{0});
}

TEST_CASE("NullBackend has no keyboard or pointer", "[hid]") {
  NullBackend backend;
  backend.OnEvent({.kind = rex::hid::InputEvent::Kind::KeyDown, .key = rex::hid::VirtualKey::kA});
  CHECK_FALSE(backend.IsKeyDown(rex::hid::VirtualKey::kA));
  CHECK_FALSE(backend.pointer().has_position);
  CHECK(backend.pointer_mode() == rex::hid::PointerMode::Free);
}
