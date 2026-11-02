/**
 * @file        tests/unit/hid/key_capture_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "../ui/ui_harness.h"

#include <algorithm>
#include <chrono>

#include <rex/hid/binding.h>

using namespace std::chrono_literals;

using rex::hid::InputSource;
using rex::hid::InputSourceType;
using rex::hid::KeyCaptureConfig;
using rex::hid::KeyModifiers;
using rex::hid::MouseButton;
using rex::hid::PadInput;
using rex::hid::VirtualKey;

namespace {

using rex::test_ui::UIApp;

void KeyDown(UIApp& app, VirtualKey key) {
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, key);
  app.Pump();
}

void KeyUp(UIApp& app, VirtualKey key) {
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyUp, key);
  app.Pump();
}

void ButtonDown(UIApp& app, MouseButton button) {
  rex::test_ui::DeliverMouseButton(rex::hid::InputEvent::Type::kButtonDown, button);
  app.Pump();
}

void ButtonUp(UIApp& app, MouseButton button) {
  rex::test_ui::DeliverMouseButton(rex::hid::InputEvent::Type::kButtonUp, button);
  app.Pump();
}

void SetWheel(UIApp& app, rex::i32 detents) {
  app.GetInput().TakeWheel();
  if (detents != 0) {
    rex::test_ui::DeliverWheel(detents);
    app.Pump();
    app.GetInput().TakeWheel();
  }
}

void RunUI(const std::function<void(UIApp&)>& body) {
  UIApp app;
  REQUIRE(app.Start());
  body(app);
  app.Shutdown();
}

constexpr u32 PadBit(PadInput button) {
  return 1u << static_cast<u32>(button);
}

const std::chrono::steady_clock::time_point kStart{};

}  // namespace

REX_TEST_CASE("KeyCapture bindable keys ascend and each one has a name", "[hid][key_capture]") {
  const auto keys = rex::hid::GetBindableKeys();
  REQUIRE_FALSE(keys.empty());
  for (size_t i = 0; i < keys.size(); ++i) {
    INFO(i);
    if (i > 0) {
      CHECK(static_cast<rex::u16>(keys[i - 1]) < static_cast<rex::u16>(keys[i]));
    }
    InputSource source;
    source.code = static_cast<rex::u16>(keys[i]);
    const std::string text = rex::hid::InputSourceToString(source);
    CHECK_FALSE(text.empty());
    auto parsed = rex::hid::ParseInputSource(text);
    REQUIRE(parsed);
    CHECK(*parsed == source);
  }
}

REX_TEST_CASE("KeyCapture reports a key once it is released", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    CHECK_FALSE(capture.Poll(kStart));
    capture.Begin();
    CHECK_FALSE(capture.Poll(kStart));
    CHECK_FALSE(capture.IsPending());
    KeyDown(app, VirtualKey::kW);
    CHECK_FALSE(capture.Poll(kStart));
    CHECK(capture.IsPending());
    CHECK_FALSE(capture.Poll(kStart + 10ms));
    KeyUp(app, VirtualKey::kW);
    const auto source = capture.Poll(kStart + 20ms);
    REQUIRE(source);
    CHECK(source->type == InputSourceType::kKey);
    CHECK(source->code == static_cast<rex::u16>(VirtualKey::kW));
    CHECK(source->modifiers == KeyModifiers::kNone);
    CHECK_FALSE(capture.IsPending());
    CHECK_FALSE(capture.WasCanceled());
    KeyDown(app, VirtualKey::kQ);
    CHECK_FALSE(capture.Poll(kStart + 30ms));
    CHECK_FALSE(capture.IsPending());
  });
}

REX_TEST_CASE("KeyCapture ignores a key held at the start until it is pressed again",
              "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    KeyDown(app, VirtualKey::kSpace);
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    CHECK_FALSE(capture.Poll(kStart));
    CHECK_FALSE(capture.IsPending());
    KeyUp(app, VirtualKey::kSpace);
    CHECK_FALSE(capture.Poll(kStart + 5ms));
    KeyDown(app, VirtualKey::kSpace);
    CHECK_FALSE(capture.Poll(kStart + 10ms));
    CHECK(capture.IsPending());
    KeyUp(app, VirtualKey::kSpace);
    const auto source = capture.Poll(kStart + 15ms);
    REQUIRE(source);
    CHECK(source->code == static_cast<rex::u16>(VirtualKey::kSpace));
  });
}

REX_TEST_CASE("KeyCapture keeps the modifiers held at the press", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    KeyDown(app, VirtualKey::kShift);
    KeyDown(app, VirtualKey::kControl);
    KeyDown(app, VirtualKey::kW);
    CHECK_FALSE(capture.Poll(kStart));
    KeyUp(app, VirtualKey::kShift);
    KeyUp(app, VirtualKey::kControl);
    CHECK_FALSE(capture.Poll(kStart + 5ms));
    KeyUp(app, VirtualKey::kW);
    const auto source = capture.Poll(kStart + 10ms);
    REQUIRE(source);
    CHECK(source->code == static_cast<rex::u16>(VirtualKey::kW));
    CHECK(source->modifiers == (KeyModifiers::kShift | KeyModifiers::kCtrl));
  });
}

REX_TEST_CASE("KeyCapture reads mouse buttons as keys", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    ButtonDown(app, MouseButton::kRight);
    CHECK_FALSE(capture.Poll(kStart));
    ButtonUp(app, MouseButton::kRight);
    const auto right = capture.Poll(kStart + 5ms);
    REQUIRE(right);
    CHECK(right->type == InputSourceType::kKey);
    CHECK(right->code == static_cast<rex::u16>(VirtualKey::kRButton));
  });
}

REX_TEST_CASE("KeyCapture ignores a mouse button without a name", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    ButtonDown(app, MouseButton::kNone);
    ButtonUp(app, MouseButton::kNone);
    CHECK_FALSE(capture.Poll(kStart + 5ms));
  });
}

REX_TEST_CASE("KeyCapture reads the extra mouse buttons as keys", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    ButtonDown(app, MouseButton::kX1);
    CHECK_FALSE(capture.Poll(kStart));
    ButtonUp(app, MouseButton::kX1);
    const auto first = capture.Poll(kStart + 5ms);
    REQUIRE(first);
    CHECK(first->type == InputSourceType::kKey);
    CHECK(first->code == static_cast<rex::u16>(VirtualKey::kXButton1));
    capture.Begin();
    ButtonDown(app, MouseButton::kX2);
    CHECK_FALSE(capture.Poll(kStart + 10ms));
    ButtonUp(app, MouseButton::kX2);
    const auto second = capture.Poll(kStart + 15ms);
    REQUIRE(second);
    CHECK(second->code == static_cast<rex::u16>(VirtualKey::kXButton2));
  });
}

REX_TEST_CASE("KeyCapture bindable keys include the extra mouse buttons", "[hid][key_capture]") {
  const auto keys = rex::hid::GetBindableKeys();
  CHECK(std::ranges::find(keys, VirtualKey::kXButton1) != keys.end());
  CHECK(std::ranges::find(keys, VirtualKey::kXButton2) != keys.end());
}

REX_TEST_CASE("KeyCapture reports a wheel step and ignores a wheel held at the start",
              "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    SetWheel(app, 3);
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    CHECK_FALSE(capture.Poll(kStart));
    SetWheel(app, 0);
    CHECK_FALSE(capture.Poll(kStart + 5ms));
    SetWheel(app, -2);
    const auto down = capture.Poll(kStart + 10ms);
    REQUIRE(down);
    CHECK(down->type == InputSourceType::kMouseWheel);
    CHECK(down->sign == -1);
    capture.Begin();
    SetWheel(app, 1);
    const auto up = capture.Poll(kStart + 15ms);
    REQUIRE(up);
    CHECK(up->sign == 1);
  });
}

REX_TEST_CASE("KeyCapture binds a tapped Escape and cancels on a held one", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{.cancel_hold = 800ms});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    KeyDown(app, VirtualKey::kEscape);
    CHECK_FALSE(capture.Poll(kStart));
    KeyUp(app, VirtualKey::kEscape);
    const auto tapped = capture.Poll(kStart + 799ms);
    REQUIRE(tapped);
    CHECK(tapped->code == static_cast<rex::u16>(VirtualKey::kEscape));
    CHECK_FALSE(capture.WasCanceled());

    capture.Begin();
    KeyDown(app, VirtualKey::kEscape);
    CHECK_FALSE(capture.Poll(kStart + 1s));
    KeyUp(app, VirtualKey::kEscape);
    CHECK_FALSE(capture.Poll(kStart + 1s + 800ms));
    CHECK(capture.WasCanceled());
    CHECK_FALSE(capture.IsPending());
    capture.Begin();
    CHECK_FALSE(capture.WasCanceled());
  });
}

REX_TEST_CASE("KeyCapture reports a pad button on release", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    capture.SetPadButtons(PadBit(PadInput::kA));
    CHECK_FALSE(capture.Poll(kStart));
    CHECK(capture.IsPending());
    capture.SetPadButtons(0);
    const auto source = capture.Poll(kStart + 5ms);
    REQUIRE(source);
    CHECK(source->type == InputSourceType::kPadButton);
    CHECK(source->code == static_cast<rex::u16>(PadInput::kA));
    CHECK(source->modifiers == KeyModifiers::kNone);
  });
}

REX_TEST_CASE("KeyCapture cancels when the pad cancel buttons are held", "[hid][key_capture]") {
  RunUI([](UIApp& app) {
    rex::hid::IInputSystem& input = app.GetInput();
    auto capture_owner = input.CreateKeyCapture(KeyCaptureConfig{});
    rex::hid::IKeyCapture& capture = *capture_owner;
    capture.Begin();
    KeyDown(app, VirtualKey::kW);
    CHECK_FALSE(capture.Poll(kStart));
    capture.SetPadButtons(PadBit(PadInput::kLeftTrigger));
    CHECK_FALSE(capture.Poll(kStart + 5ms));
    CHECK(capture.IsPending());
    capture.SetPadButtons(PadBit(PadInput::kLeftTrigger) | PadBit(PadInput::kRightTrigger));
    CHECK_FALSE(capture.Poll(kStart + 10ms));
    CHECK(capture.WasCanceled());
    CHECK_FALSE(capture.IsPending());

    auto without_cancel_owner = input.CreateKeyCapture(KeyCaptureConfig{.pad_cancel_mask = 0});
    rex::hid::IKeyCapture& without_cancel = *without_cancel_owner;
    without_cancel.SetPadButtons(PadBit(PadInput::kLeftTrigger) | PadBit(PadInput::kRightTrigger));
    without_cancel.Begin();
    KeyUp(app, VirtualKey::kW);
    KeyDown(app, VirtualKey::kQ);
    CHECK_FALSE(without_cancel.Poll(kStart + 20ms));
    KeyUp(app, VirtualKey::kQ);
    const auto source = without_cancel.Poll(kStart + 25ms);
    REQUIRE(source);
    CHECK_FALSE(without_cancel.WasCanceled());
  });
}
