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

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <rex/hid.h>

#include "core/cvar_registry.h"
#include "hid/common/cvar_table.h"
#include "hid/common/hub.h"
#include "hid/common/keyboard_mouse_driver.h"

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
  bool Post(std::move_only_function<void()> work) override {
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
  bool capture_active() const override { return capture; }

  void RunPosted() {
    auto work = std::move(posted);
    posted.clear();
    for (auto& item : work) {
      item();
    }
  }

  std::vector<std::move_only_function<void()>> posted;
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
    out.push_back(info);
  }
  rex::Result<rex::hid::X_INPUT_STATE> GetDeviceState(rex::hid::DeviceId) override {
    rex::hid::X_INPUT_STATE state{};
    state.gamepad.buttons = buttons;
    return state;
  }
  rex::Result<rex::hid::X_INPUT_CAPABILITIES> GetDeviceCapabilities(rex::hid::DeviceId,
                                                                    rex::u32) override {
    return rex::hid::X_INPUT_CAPABILITIES{};
  }
  rex::Result<void> SetDeviceVibration(rex::hid::DeviceId,
                                       const rex::hid::X_INPUT_VIBRATION&) override {
    return rex::Ok();
  }
  rex::Result<rex::hid::X_INPUT_KEYSTROKE> GetDeviceKeystroke(rex::hid::DeviceId,
                                                              rex::u32) override {
    return std::unexpected(rex::hid::MakeError(rex::hid::Error::Empty, "none"));
  }
  rex::u16 buttons = 0;
};

InputEvent Key(Kind kind, VirtualKey key) {
  return {.kind = kind, .key = key};
}

InputEvent Motion(rex::f32 x, rex::f32 y, rex::f32 dx = 0.0f, rex::f32 dy = 0.0f) {
  return {.kind = Kind::Motion, .x = x, .y = y, .dx = dx, .dy = dy};
}

}  // namespace

TEST_CASE("Hub keeps keyboard state and modifiers", "[hid]") {
  Hub hub;
  hub.OnEvent(Key(Kind::KeyDown, VirtualKey::kA));
  hub.OnEvent(Key(Kind::KeyDown, VirtualKey::kShift));
  CHECK(hub.IsKeyDown(VirtualKey::kA));
  CHECK(hub.AnyKeyDown());
  CHECK(hub.modifiers() == KeyModifiers::Shift);
  hub.OnEvent(Key(Kind::KeyUp, VirtualKey::kA));
  hub.OnEvent(Key(Kind::KeyUp, VirtualKey::kShift));
  CHECK_FALSE(hub.AnyKeyDown());
}

TEST_CASE("Hub tracks the free pointer and consumes the moved flag", "[hid]") {
  Hub hub;
  hub.OnEvent({.kind = Kind::Resize, .width = 100, .height = 100});
  hub.OnEvent(Motion(10.0f, 20.0f));
  CHECK(hub.pointer().has_position);
  CHECK(hub.pointer().x == 10.0f);
  CHECK(hub.pointer().window_width == 100);
  CHECK(hub.TakePointerMoved());
  CHECK_FALSE(hub.TakePointerMoved());
  CHECK(hub.TakeMotion().dx == 0.0f);
}

TEST_CASE("Hub posts a pointer mode once and applies the last one", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetHost(&host);
  hub.SetPointerMode(PointerMode::Hidden);
  hub.SetPointerMode(PointerMode::Locked);
  CHECK(host.posted.size() == 1);
  host.RunPosted();
  CHECK(host.applied == std::vector<PointerMode>{PointerMode::Locked});
  hub.SetHost(nullptr);
}

TEST_CASE("Hub posts again after the host dropped a pointer update", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetHost(&host);
  host.accept = false;
  hub.SetPointerMode(PointerMode::Hidden);
  host.accept = true;
  hub.SetPointerMode(PointerMode::Locked);
  CHECK(host.posted.size() == 1);
  host.RunPosted();
  CHECK(host.applied.back() == PointerMode::Locked);
  hub.SetHost(nullptr);
}

TEST_CASE("Hub frees the pointer while the host holds no capture", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetPointerMode(PointerMode::Hidden);
  hub.SetHost(&host);
  CHECK(host.applied.back() == PointerMode::Hidden);
  host.capture = false;
  (void)hub.GetState(0);
  host.RunPosted();
  CHECK(host.applied.back() == PointerMode::Free);
  host.capture = true;
  (void)hub.GetState(0);
  host.RunPosted();
  CHECK(host.applied.back() == PointerMode::Hidden);
  hub.SetHost(nullptr);
}

TEST_CASE("Hub applies a mode set before any host on SetHost", "[hid]") {
  Hub hub;
  hub.SetPointerMode(PointerMode::Hidden);
  FakeHost host;
  hub.SetHost(&host);
  CHECK(host.applied == std::vector<PointerMode>{PointerMode::Hidden});
  hub.SetHost(nullptr);
  CHECK(host.applied.back() == PointerMode::Free);
}

TEST_CASE("Hub accumulates locked motion, Take consumes and Peek does not", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetPointerMode(PointerMode::Locked);
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

TEST_CASE("Hub recenters a locked pointer without relative motion", "[hid]") {
  Hub hub;
  FakeHost host;
  host.relative = false;
  hub.OnEvent({.kind = Kind::Resize, .width = 100, .height = 100});
  hub.OnEvent(Motion(50.0f, 50.0f));
  hub.SetPointerMode(PointerMode::Locked);
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

TEST_CASE("Hub frees the pointer and drops held input on focus loss", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetPointerMode(PointerMode::Locked);
  hub.SetHost(&host);
  hub.OnEvent(Key(Kind::KeyDown, VirtualKey::kW));
  hub.OnEvent({.kind = Kind::ButtonDown, .button = MouseButton::Left});
  hub.OnEvent(Motion(0.0f, 0.0f, 5.0f, 5.0f));
  hub.OnEvent({.kind = Kind::FocusLost});
  CHECK_FALSE(hub.focused());
  CHECK_FALSE(hub.IsKeyDown(VirtualKey::kW));
  CHECK_FALSE(hub.AnyButtonDown());
  CHECK(host.applied.back() == PointerMode::Free);
  hub.OnEvent({.kind = Kind::FocusGained});
  CHECK(host.applied.back() == PointerMode::Locked);
  CHECK(hub.TakeMotion().dx == 0.0f);
  hub.SetHost(nullptr);
}

TEST_CASE("Hub drops events from a detached host", "[hid]") {
  Hub hub;
  FakeHost host;
  hub.SetHost(&host);
  hub.SetHost(nullptr);
  hub.SetPointerMode(PointerMode::Locked);
  CHECK(host.posted.empty());
  CHECK(host.applied.empty());
}

TEST_CASE("Hub returns the wheel detents once", "[hid]") {
  Hub hub;
  hub.OnEvent({.kind = Kind::Wheel, .wheel = 2});
  hub.OnEvent({.kind = Kind::Wheel, .wheel = -1});
  CHECK(hub.TakeWheel() == 1);
  CHECK(hub.wheel() == 1);
  CHECK(hub.TakeWheel() == 0);
}

TEST_CASE("Hub masks buttons held when the last UI block drops", "[hid]") {
  Hub hub;
  auto pad = std::make_unique<FakePad>();
  FakePad* raw = pad.get();
  hub.AddPadDriver(std::move(pad));
  raw->buttons = rex::hid::X_INPUT_GAMEPAD_A;
  hub.AcquireUIBlock();
  auto blocked = hub.GetState(0);
  REQUIRE(blocked);
  CHECK(static_cast<rex::u16>(blocked->gamepad.buttons) == 0);
  hub.ReleaseUIBlock();
  CHECK(static_cast<rex::u16>(hub.GetState(0)->gamepad.buttons) == 0);
  raw->buttons = 0;
  (void)hub.GetState(0);
  raw->buttons = rex::hid::X_INPUT_GAMEPAD_A;
  CHECK(static_cast<rex::u16>(hub.GetState(0)->gamepad.buttons) == rex::hid::X_INPUT_GAMEPAD_A);
}

TEST_CASE("Hub tells the listener when user 0 connects", "[hid]") {
  Hub hub;
  std::vector<std::pair<rex::u32, bool>> events;
  hub.SetConnectionListener([&events](rex::u32 user, bool on) { events.emplace_back(user, on); });
  (void)hub.GetState(0);
  (void)hub.GetState(0);
  CHECK(events == std::vector<std::pair<rex::u32, bool>>{{0, true}});
}

TEST_CASE("Keyboard pad releases bound keys and drops motion on focus loss", "[hid]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("hid", rex::hid::cvars::kHIDCvars));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mode, true));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mouse, true));
  Hub hub;
  rex::hid::KeyboardMouseDriver pad(hub);
  std::vector<rex::hid::DeviceInfo> devices;
  pad.EnumerateDevices(devices);
  REQUIRE(devices.size() == 1);
  const auto id = devices[0].id;

  pad.OnEvent(Key(Kind::KeyDown, VirtualKey::kSpace));
  pad.OnEvent(Motion(0.0f, 0.0f, 40.0f, 0.0f));
  auto down = pad.GetDeviceKeystroke(id, 0);
  REQUIRE(down);
  CHECK((down->flags & rex::hid::X_INPUT_KEYSTROKE_KEYUP) == 0);

  hub.OnEvent({.kind = Kind::FocusLost});
  pad.OnEvent({.kind = Kind::FocusLost});
  auto up = pad.GetDeviceKeystroke(id, 0);
  REQUIRE(up);
  CHECK(up->virtual_key == down->virtual_key);
  CHECK((up->flags & rex::hid::X_INPUT_KEYSTROKE_KEYUP) != 0);

  hub.OnEvent({.kind = Kind::FocusGained});
  auto state = pad.GetDeviceState(id);
  REQUIRE(state);
  CHECK(static_cast<rex::u16>(state->gamepad.buttons) == 0);
  CHECK(static_cast<rex::i16>(state->gamepad.thumb_rx) == 0);

  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mouse, false));
  REQUIRE(REXCVAR_SET(rex::hid::cvars::mnk_mode, false));
  registry->Unregister(rex::hid::cvars::kHIDCvars);
}
