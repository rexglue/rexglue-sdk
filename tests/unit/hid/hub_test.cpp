/**
 * @file        tests/unit/hid/hub_test.cpp
 * @brief       hid::Hub keyboard, pointer, pointer mode and UI block behavior
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hid.h>
#include <rex/move_only_function.h>

#include "core/cvar_registry.h"
#include "hid/cvar_table.h"
#include "hid/hub.h"
#include "hid/keyboard_mouse_driver.h"

using rex::hid::Hub;
using rex::hid::InputEvent;
using rex::hid::KeyModifiers;
using rex::hid::MouseButton;
using rex::hid::PointerMode;
using rex::hid::PointerState;
using rex::hid::VirtualKey;
using Kind = rex::hid::InputEvent::Kind;

namespace {

class FakeHost final : public rex::hid::IInputHost {
 public:
  bool Post(rex::MoveOnlyFunction<void()> work) override {
    if (!accept) {
      return false;
    }
    posted.push_back(std::move(work));
    return true;
  }
  bool ApplyPointerMode(PointerMode mode) override {
    applied.push_back(mode);
    return relative;
  }
  std::optional<PointerState> WarpPointerToCenter() override {
    ++warps;
    return PointerState{.x = 50.0f, .y = 50.0f, .window_width = 100, .window_height = 100};
  }
  bool GetCaptureActive() const override { return capture; }

  void RunPosted() {
    auto work = std::move(posted);
    posted.clear();
    for (auto& item : work) {
      item();
    }
  }

  std::vector<rex::MoveOnlyFunction<void()>> posted;
  std::vector<PointerMode> applied;
  bool relative = true;
  bool accept = true;
  bool capture = true;
  int warps = 0;
};

class FakePad final : public rex::hid::Driver {
 public:
  void EnumerateDevices(std::vector<rex::hid::DeviceInfo>& out) override {
    rex::hid::DeviceInfo info;
    info.id = static_cast<rex::hid::DeviceId>(7);
    info.name = "Fake controller with a name past the small string buffer";
    out.push_back(info);
  }
  [[nodiscard]] rex::Result<rex::hid::X_INPUT_STATE> GetDeviceState(rex::hid::DeviceId) override {
    rex::hid::X_INPUT_STATE state{};
    state.gamepad.buttons = buttons;
    return state;
  }
  [[nodiscard]] rex::Result<rex::hid::X_INPUT_CAPABILITIES> GetDeviceCapabilities(
      rex::hid::DeviceId, rex::u32) override {
    return rex::hid::X_INPUT_CAPABILITIES{};
  }
  [[nodiscard]] rex::Result<void> SetDeviceVibration(rex::hid::DeviceId,
                                                     const rex::hid::X_INPUT_VIBRATION&) override {
    return rex::Ok();
  }
  [[nodiscard]] rex::Result<rex::hid::X_INPUT_KEYSTROKE> GetDeviceKeystroke(rex::hid::DeviceId,
                                                                            rex::u32) override {
    return std::unexpected(rex::hid::BuildError(rex::hid::HIDError::kEmpty, "none"));
  }
  rex::u16 buttons = 0;
};

InputEvent Key(Kind kind, VirtualKey key) {
  return {.kind = kind, .key = key};
}

InputEvent Motion(rex::f32 x, rex::f32 y, rex::f32 dx = 0.0f, rex::f32 dy = 0.0f) {
  return {.kind = Kind::kMotion, .x = x, .y = y, .dx = dx, .dy = dy};
}

}  // namespace

REX_TEST_CASE("Hub keeps keyboard state and modifiers", "[hid]") {
  Hub hub;
  hub.OnEvent(Key(Kind::kKeyDown, VirtualKey::kA));
  hub.OnEvent(Key(Kind::kKeyDown, VirtualKey::kShift));
  CHECK(hub.IsKeyDown(VirtualKey::kA));
  CHECK(hub.IsAnyKeyDown());
  CHECK(hub.GetModifiers() == KeyModifiers::kShift);
  hub.OnEvent(Key(Kind::kKeyUp, VirtualKey::kA));
  hub.OnEvent(Key(Kind::kKeyUp, VirtualKey::kShift));
  CHECK_FALSE(hub.IsAnyKeyDown());
}

REX_TEST_CASE("Hub tracks the free pointer and consumes the moved flag", "[hid]") {
  Hub hub;
  hub.OnEvent({.kind = Kind::kResize, .width = 100, .height = 100});
  hub.OnEvent(Motion(10.0f, 20.0f));
  CHECK(hub.GetPointer().has_position);
  CHECK(hub.GetPointer().x == 10.0f);
  CHECK(hub.GetPointer().window_width == 100);
  CHECK(hub.TakePointerMoved());
  CHECK_FALSE(hub.TakePointerMoved());
  CHECK(hub.TakeMotion().dx == 0.0f);
}

REX_TEST_CASE("Hub posts a pointer mode once and applies the last one", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetHost(&host);
  hub.SetPointerMode(PointerMode::kHidden);
  hub.SetPointerMode(PointerMode::kLocked);
  CHECK(host.posted.size() == 1);
  host.RunPosted();
  CHECK(host.applied == std::vector<PointerMode>{PointerMode::kLocked});
  hub.SetHost(nullptr);
}

REX_TEST_CASE("Hub posts again after the host dropped a pointer update", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetHost(&host);
  host.accept = false;
  hub.SetPointerMode(PointerMode::kHidden);
  host.accept = true;
  hub.SetPointerMode(PointerMode::kLocked);
  CHECK(host.posted.size() == 1);
  host.RunPosted();
  CHECK(host.applied.back() == PointerMode::kLocked);
  hub.SetHost(nullptr);
}

REX_TEST_CASE("Hub frees the pointer while the host holds no capture", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetPointerMode(PointerMode::kHidden);
  hub.SetHost(&host);
  CHECK(host.applied.back() == PointerMode::kHidden);
  host.capture = false;
  CHECK(hub.GetState(0));
  host.RunPosted();
  CHECK(host.applied.back() == PointerMode::kFree);
  host.capture = true;
  CHECK(hub.GetState(0));
  host.RunPosted();
  CHECK(host.applied.back() == PointerMode::kHidden);
  hub.SetHost(nullptr);
}

REX_TEST_CASE("Hub applies a mode set before any host on SetHost", "[hid]") {
  Hub hub;
  hub.SetPointerMode(PointerMode::kHidden);
  FakeHost host;
  hub.SetHost(&host);
  CHECK(host.applied == std::vector<PointerMode>{PointerMode::kHidden});
  hub.SetHost(nullptr);
  CHECK(host.applied.back() == PointerMode::kFree);
}

REX_TEST_CASE("Hub accumulates locked motion, Take consumes and Peek does not", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetPointerMode(PointerMode::kLocked);
  hub.SetHost(&host);
  hub.OnEvent(Motion(0.0f, 0.0f, 3.0f, -2.0f));
  hub.OnEvent(Motion(0.0f, 0.0f, 1.0f, 1.0f));
  CHECK(hub.PeekMotion().dx == 4.0f);
  CHECK(hub.PeekMotion().dy == -1.0f);
  const auto taken = hub.TakeMotion();
  CHECK(taken.dx == 4.0f);
  CHECK(hub.TakeMotion().dx == 0.0f);
  hub.SetHost(nullptr);
}

REX_TEST_CASE("Hub recenters a locked pointer without relative motion", "[hid]") {
  Hub hub;
  FakeHost host;
  host.relative = false;
  hub.OnEvent({.kind = Kind::kResize, .width = 100, .height = 100});
  hub.OnEvent(Motion(50.0f, 50.0f));
  hub.SetPointerMode(PointerMode::kLocked);
  hub.SetHost(&host);
  hub.OnEvent(Motion(60.0f, 50.0f));
  CHECK(hub.TakeMotion().dx == 10.0f);
  CHECK(host.warps == 0);
  hub.OnEvent(Motion(95.0f, 50.0f));
  CHECK(host.warps == 1);
  hub.OnEvent(Motion(52.0f, 50.0f));
  CHECK(hub.TakeMotion().dx == 35.0f + 2.0f);
  hub.SetHost(nullptr);
}

REX_TEST_CASE("Hub frees the pointer and drops held input on focus loss", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetPointerMode(PointerMode::kLocked);
  hub.SetHost(&host);
  hub.OnEvent(Key(Kind::kKeyDown, VirtualKey::kW));
  hub.OnEvent({.kind = Kind::kButtonDown, .button = MouseButton::kLeft});
  hub.OnEvent(Motion(0.0f, 0.0f, 5.0f, 5.0f));
  hub.OnEvent({.kind = Kind::kFocusLost});
  CHECK_FALSE(hub.GetFocused());
  CHECK_FALSE(hub.IsKeyDown(VirtualKey::kW));
  CHECK_FALSE(hub.IsAnyButtonDown());
  CHECK(host.applied.back() == PointerMode::kFree);
  hub.OnEvent({.kind = Kind::kFocusGained});
  CHECK(host.applied.back() == PointerMode::kLocked);
  CHECK(hub.TakeMotion().dx == 0.0f);
  hub.SetHost(nullptr);
}

REX_TEST_CASE("Hub drops events from a detached host", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetHost(&host);
  hub.SetHost(nullptr);
  hub.SetPointerMode(PointerMode::kLocked);
  CHECK(host.posted.empty());
  CHECK(host.applied.empty());
}

REX_TEST_CASE("Hub returns the wheel detents once", "[hid]") {
  Hub hub;
  hub.OnEvent({.kind = Kind::kWheel, .wheel = 2});
  hub.OnEvent({.kind = Kind::kWheel, .wheel = -1});
  CHECK(hub.TakeWheel() == 1);
  CHECK(hub.GetWheel() == 1);
  CHECK(hub.TakeWheel() == 0);
}

REX_TEST_CASE("Hub masks buttons held when the last UI block drops", "[hid]") {
  Hub hub;
  auto pad = std::make_unique<FakePad>();
  FakePad* raw = pad.get();
  hub.AddPadDriver(std::move(pad));
  raw->buttons = rex::hid::kXInputGamepadA;
  hub.AcquireUIBlock();
  auto blocked = hub.GetState(0);
  REQUIRE(blocked);
  CHECK(static_cast<rex::u16>(blocked->gamepad.buttons) == 0);
  hub.ReleaseUIBlock();
  CHECK(static_cast<rex::u16>(hub.GetState(0)->gamepad.buttons) == 0);
  raw->buttons = 0;
  CHECK(hub.GetState(0));
  raw->buttons = rex::hid::kXInputGamepadA;
  CHECK(static_cast<rex::u16>(hub.GetState(0)->gamepad.buttons) == rex::hid::kXInputGamepadA);
}

REX_TEST_CASE("Hub tells the listener when user 0 connects", "[hid]") {
  Hub hub;
  std::vector<std::pair<rex::u32, bool>> events;
  hub.SetConnectionListener([&events](rex::u32 user, bool on) { events.emplace_back(user, on); });
  CHECK(hub.GetState(0));
  CHECK(hub.GetState(0));
  CHECK(events == std::vector<std::pair<rex::u32, bool>>{{0, true}});
}

REX_TEST_CASE("Keyboard pad releases bound keys and drops motion on focus loss", "[hid]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("hid", rex::hid::cvars::kHIDCvars));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mode, true));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mouse, true));
  Hub hub;
  rex::hid::KeyboardMouseDriver pad(hub);
  pad.Attach(*registry);
  std::vector<rex::hid::DeviceInfo> devices;
  pad.EnumerateDevices(devices);
  REQUIRE(devices.size() == 1);
  const auto id = devices[0].id;

  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kSpace));
  pad.OnEvent(Motion(0.0f, 0.0f, 40.0f, 0.0f));
  auto down = pad.GetDeviceKeystroke(id, 0);
  REQUIRE(down);
  CHECK((down->flags & rex::hid::kXInputKeystrokeKeyUp) == 0);

  hub.OnEvent({.kind = Kind::kFocusLost});
  pad.OnEvent({.kind = Kind::kFocusLost});
  auto up = pad.GetDeviceKeystroke(id, 0);
  REQUIRE(up);
  CHECK(up->virtual_key == down->virtual_key);
  CHECK((up->flags & rex::hid::kXInputKeystrokeKeyUp) != 0);

  hub.OnEvent({.kind = Kind::kFocusGained});
  auto state = pad.GetDeviceState(id);
  REQUIRE(state);
  CHECK(static_cast<rex::u16>(state->gamepad.buttons) == 0);
  CHECK(static_cast<rex::i16>(state->gamepad.thumb_rx) == 0);

  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mouse, false));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mode, false));
  registry->Unregister(rex::hid::cvars::kHIDCvars);
}

namespace {

std::vector<rex::u16> Drain(rex::hid::KeyboardMouseDriver& pad, rex::hid::DeviceId id) {
  std::vector<rex::u16> keys;
  while (auto stroke = pad.GetDeviceKeystroke(id, 0)) {
    keys.push_back(static_cast<rex::u16>(stroke->virtual_key));
  }
  return keys;
}

}  // namespace

REX_TEST_CASE("Keyboard pad binds fire only with their exact modifiers and follow cvar changes",
              "[hid][key_chord]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("hid", rex::hid::cvars::kHIDCvars));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mode, true));
  Hub hub;
  rex::hid::KeyboardMouseDriver pad(hub);
  pad.Attach(*registry);
  std::vector<rex::hid::DeviceInfo> devices;
  pad.EnumerateDevices(devices);
  REQUIRE(devices.size() == 1);
  const auto id = devices[0].id;
  const auto dpad_up = std::to_underlying(VirtualKey::kXInputPadDpadUp);
  const auto rstick_up = std::to_underlying(VirtualKey::kXInputPadRThumbUp);
  const auto pad_x = std::to_underlying(VirtualKey::kXInputPadX);

  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kShift));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kUp));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{dpad_up});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kUp));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{dpad_up});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kShift));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kUp));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{rstick_up});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kUp));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{rstick_up});

  REQUIRE(registry->Set("hid.keybind_x", "Ctrl+J"));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kJ));
  CHECK(Drain(pad, id).empty());
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kJ));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kControl));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kJ));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{pad_x});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kJ));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{pad_x});

  REQUIRE(registry->Set("hid.keybind_x", "Ctrl+NoSuchKey"));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kJ));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{pad_x});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kJ));
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kControl));
  Drain(pad, id);

  REQUIRE(registry->Set("hid.keybind_x", "NoSuchKey, J"));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kJ));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{pad_x});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kJ));
  Drain(pad, id);

  REQUIRE(registry->Set("hid.keybind_x", "K,\r\nJ\r\n"));
  pad.OnEvent(Key(Kind::kKeyDown, VirtualKey::kJ));
  CHECK(Drain(pad, id) == std::vector<rex::u16>{pad_x});
  pad.OnEvent(Key(Kind::kKeyUp, VirtualKey::kJ));
  Drain(pad, id);

  REQUIRE(registry->ResetToDefault("hid.keybind_x"));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mode, false));
  registry->Unregister(rex::hid::cvars::kHIDCvars);
}

REX_TEST_CASE("Hub serves polls from 4 threads at once", "[hid]") {
  Hub hub;
  hub.AddPadDriver(std::make_unique<FakePad>());
  constexpr int kThreads = 4;
  constexpr int kIterations = 2000;
  std::atomic<int> answered{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&hub, &answered] {
      for (int i = 0; i < kIterations; ++i) {
        int count = 0;
        count += hub.GetState(0).has_value();
        count += hub.GetCapabilities(0, 0).has_value();
        count += hub.SetVibration(0, rex::hid::X_INPUT_VIBRATION{}).has_value();
        count += hub.GetKeystroke(0, 0).has_value();
        answered.fetch_add(count);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(answered.load() >= kThreads * kIterations * 3);
  CHECK(hub.GetState(0));
}
