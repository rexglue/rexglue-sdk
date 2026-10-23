/**
 * @file        tests/unit/hid/key_capture_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <chrono>
#include <set>

#include <rex/hid/key_capture.h>

using namespace std::chrono_literals;

using rex::hid::InputSource;
using rex::hid::InputSourceKind;
using rex::hid::KeyCapture;
using rex::hid::KeyCaptureConfig;
using rex::hid::KeyModifiers;
using rex::hid::MouseButton;
using rex::hid::PadButton;
using rex::hid::VirtualKey;

namespace {

class FakeInput final : public rex::hid::IHIDBackend {
 public:
  rex::Result<rex::hid::X_INPUT_CAPABILITIES> GetCapabilities(rex::u32, rex::u32) override {
    return rex::Err<rex::hid::X_INPUT_CAPABILITIES>(rex::ErrorCategory::kNotImplemented, "fake");
  }
  rex::Result<rex::hid::PadInfo> GetPadInfo(rex::u32) override {
    return rex::Err<rex::hid::PadInfo>(rex::ErrorCategory::kNotImplemented, "fake");
  }
  rex::Result<rex::hid::X_INPUT_STATE> GetState(rex::u32) override {
    return rex::Err<rex::hid::X_INPUT_STATE>(rex::ErrorCategory::kNotImplemented, "fake");
  }
  rex::Result<void> SetVibration(rex::u32, const rex::hid::X_INPUT_VIBRATION&) override {
    return rex::Err<void>(rex::ErrorCategory::kNotImplemented, "fake");
  }
  rex::Result<rex::hid::X_INPUT_KEYSTROKE> GetKeystroke(rex::u32, rex::u32) override {
    return rex::Err<rex::hid::X_INPUT_KEYSTROKE>(rex::ErrorCategory::kNotImplemented, "fake");
  }
  void AcquireUIBlock() override {}
  void ReleaseUIBlock() override {}
  void SetConnectionListener(rex::MoveOnlyFunction<void(rex::u32, bool)>) override {}
  void SetHost(rex::hid::IInputHost*) override {}
  void OnEvent(const rex::hid::InputEvent&) override {}
  bool IsKeyDown(VirtualKey key) const override { return keys.contains(key); }
  bool IsAnyKeyDown() const override { return !keys.empty(); }
  KeyModifiers GetModifiers() const override { return modifiers; }
  bool IsFocused() const override { return true; }
  rex::hid::PointerState GetPointer() const override { return {}; }
  bool IsButtonDown(MouseButton button) const override { return buttons.contains(button); }
  bool IsAnyButtonDown() const override { return !buttons.empty(); }
  bool TakePointerMoved() override { return false; }
  rex::hid::PointerDelta TakeMotion() override { return {}; }
  rex::hid::PointerDelta PeekMotion() const override { return {}; }
  rex::i32 TakeWheel() override { return 0; }
  rex::i32 GetWheel() const override { return wheel; }
  void SetPointerMode(rex::hid::PointerMode) override {}
  rex::hid::PointerMode GetPointerMode() const override { return rex::hid::PointerMode::kFree; }

  std::set<VirtualKey> keys;
  std::set<MouseButton> buttons;
  KeyModifiers modifiers = KeyModifiers::kNone;
  rex::i32 wheel = 0;
};

constexpr u32 PadBit(PadButton button) {
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
  FakeInput input;
  KeyCapture capture(input);
  CHECK_FALSE(capture.Poll(kStart));
  capture.Begin();
  CHECK_FALSE(capture.Poll(kStart));
  CHECK_FALSE(capture.IsPending());
  input.keys.insert(VirtualKey::kW);
  CHECK_FALSE(capture.Poll(kStart));
  CHECK(capture.IsPending());
  CHECK_FALSE(capture.Poll(kStart + 10ms));
  input.keys.clear();
  const auto source = capture.Poll(kStart + 20ms);
  REQUIRE(source);
  CHECK(source->kind == InputSourceKind::kKey);
  CHECK(source->code == static_cast<rex::u16>(VirtualKey::kW));
  CHECK(source->modifiers == KeyModifiers::kNone);
  CHECK_FALSE(capture.IsPending());
  CHECK_FALSE(capture.WasCanceled());
  input.keys.insert(VirtualKey::kQ);
  CHECK_FALSE(capture.Poll(kStart + 30ms));
  CHECK_FALSE(capture.IsPending());
}

REX_TEST_CASE("KeyCapture ignores a key held at the start until it is pressed again",
              "[hid][key_capture]") {
  FakeInput input;
  input.keys.insert(VirtualKey::kSpace);
  KeyCapture capture(input);
  capture.Begin();
  CHECK_FALSE(capture.Poll(kStart));
  CHECK_FALSE(capture.IsPending());
  input.keys.clear();
  CHECK_FALSE(capture.Poll(kStart + 5ms));
  input.keys.insert(VirtualKey::kSpace);
  CHECK_FALSE(capture.Poll(kStart + 10ms));
  CHECK(capture.IsPending());
  input.keys.clear();
  const auto source = capture.Poll(kStart + 15ms);
  REQUIRE(source);
  CHECK(source->code == static_cast<rex::u16>(VirtualKey::kSpace));
}

REX_TEST_CASE("KeyCapture keeps the modifiers held at the press", "[hid][key_capture]") {
  FakeInput input;
  KeyCapture capture(input);
  capture.Begin();
  input.modifiers = KeyModifiers::kShift | KeyModifiers::kCtrl;
  input.keys.insert(VirtualKey::kW);
  CHECK_FALSE(capture.Poll(kStart));
  input.modifiers = KeyModifiers::kNone;
  CHECK_FALSE(capture.Poll(kStart + 5ms));
  input.keys.clear();
  const auto source = capture.Poll(kStart + 10ms);
  REQUIRE(source);
  CHECK(source->code == static_cast<rex::u16>(VirtualKey::kW));
  CHECK(source->modifiers == (KeyModifiers::kShift | KeyModifiers::kCtrl));
}

REX_TEST_CASE("KeyCapture reads mouse buttons as keys", "[hid][key_capture]") {
  FakeInput input;
  KeyCapture capture(input);
  capture.Begin();
  input.buttons.insert(MouseButton::kRight);
  CHECK_FALSE(capture.Poll(kStart));
  input.buttons.clear();
  const auto right = capture.Poll(kStart + 5ms);
  REQUIRE(right);
  CHECK(right->kind == InputSourceKind::kKey);
  CHECK(right->code == static_cast<rex::u16>(VirtualKey::kRButton));
}

REX_TEST_CASE("KeyCapture reads the extra mouse buttons as keys", "[hid][key_capture]") {
  FakeInput input;
  KeyCapture capture(input);
  capture.Begin();
  input.buttons.insert(MouseButton::kX1);
  CHECK_FALSE(capture.Poll(kStart));
  input.buttons.clear();
  const auto first = capture.Poll(kStart + 5ms);
  REQUIRE(first);
  CHECK(first->kind == InputSourceKind::kKey);
  CHECK(first->code == static_cast<rex::u16>(VirtualKey::kXButton1));
  capture.Begin();
  input.buttons.insert(MouseButton::kX2);
  CHECK_FALSE(capture.Poll(kStart + 10ms));
  input.buttons.clear();
  const auto second = capture.Poll(kStart + 15ms);
  REQUIRE(second);
  CHECK(second->code == static_cast<rex::u16>(VirtualKey::kXButton2));
}

REX_TEST_CASE("KeyCapture bindable keys include the extra mouse buttons", "[hid][key_capture]") {
  const auto keys = rex::hid::GetBindableKeys();
  CHECK(std::ranges::find(keys, VirtualKey::kXButton1) != keys.end());
  CHECK(std::ranges::find(keys, VirtualKey::kXButton2) != keys.end());
}

REX_TEST_CASE("KeyCapture reports a wheel step and ignores a wheel held at the start",
              "[hid][key_capture]") {
  FakeInput input;
  input.wheel = 3;
  KeyCapture capture(input);
  capture.Begin();
  CHECK_FALSE(capture.Poll(kStart));
  input.wheel = 0;
  CHECK_FALSE(capture.Poll(kStart + 5ms));
  input.wheel = -2;
  const auto down = capture.Poll(kStart + 10ms);
  REQUIRE(down);
  CHECK(down->kind == InputSourceKind::kMouseWheel);
  CHECK(down->sign == -1);
  capture.Begin();
  input.wheel = 1;
  const auto up = capture.Poll(kStart + 15ms);
  REQUIRE(up);
  CHECK(up->sign == 1);
}

REX_TEST_CASE("KeyCapture binds a tapped Escape and cancels on a held one", "[hid][key_capture]") {
  FakeInput input;
  KeyCapture capture(input, KeyCaptureConfig{.cancel_hold = 800ms});
  capture.Begin();
  input.keys.insert(VirtualKey::kEscape);
  CHECK_FALSE(capture.Poll(kStart));
  input.keys.clear();
  const auto tapped = capture.Poll(kStart + 799ms);
  REQUIRE(tapped);
  CHECK(tapped->code == static_cast<rex::u16>(VirtualKey::kEscape));
  CHECK_FALSE(capture.WasCanceled());

  capture.Begin();
  input.keys.insert(VirtualKey::kEscape);
  CHECK_FALSE(capture.Poll(kStart + 1s));
  input.keys.clear();
  CHECK_FALSE(capture.Poll(kStart + 1s + 800ms));
  CHECK(capture.WasCanceled());
  CHECK_FALSE(capture.IsPending());
  capture.Begin();
  CHECK_FALSE(capture.WasCanceled());
}

REX_TEST_CASE("KeyCapture reports a pad button on release", "[hid][key_capture]") {
  FakeInput input;
  KeyCapture capture(input);
  capture.Begin();
  capture.SetPadButtons(PadBit(PadButton::kA));
  CHECK_FALSE(capture.Poll(kStart));
  CHECK(capture.IsPending());
  capture.SetPadButtons(0);
  const auto source = capture.Poll(kStart + 5ms);
  REQUIRE(source);
  CHECK(source->kind == InputSourceKind::kPadButton);
  CHECK(source->code == static_cast<rex::u16>(PadButton::kA));
  CHECK(source->modifiers == KeyModifiers::kNone);
}

REX_TEST_CASE("KeyCapture cancels when the pad cancel buttons are held", "[hid][key_capture]") {
  FakeInput input;
  KeyCapture capture(input);
  capture.Begin();
  input.keys.insert(VirtualKey::kW);
  CHECK_FALSE(capture.Poll(kStart));
  capture.SetPadButtons(PadBit(PadButton::kLeftTrigger));
  CHECK_FALSE(capture.Poll(kStart + 5ms));
  CHECK(capture.IsPending());
  capture.SetPadButtons(PadBit(PadButton::kLeftTrigger) | PadBit(PadButton::kRightTrigger));
  CHECK_FALSE(capture.Poll(kStart + 10ms));
  CHECK(capture.WasCanceled());
  CHECK_FALSE(capture.IsPending());

  KeyCapture without_cancel(input, KeyCaptureConfig{.pad_cancel_mask = 0});
  without_cancel.SetPadButtons(PadBit(PadButton::kLeftTrigger) | PadBit(PadButton::kRightTrigger));
  without_cancel.Begin();
  input.keys.clear();
  input.keys.insert(VirtualKey::kQ);
  CHECK_FALSE(without_cancel.Poll(kStart + 20ms));
  input.keys.clear();
  const auto source = without_cancel.Poll(kStart + 25ms);
  REQUIRE(source);
  CHECK_FALSE(without_cancel.WasCanceled());
}
