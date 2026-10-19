/**
 * @file        tests/unit/hid/input_assignment_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <map>
#include <memory>
#include <vector>

#include <rex/hid.h>

#include "hid/device_assignment.h"
#include "hid/hub.h"

using rex::hid::CodeOf;
using rex::hid::Device;
using rex::hid::DeviceId;
using rex::hid::Driver;
using rex::hid::HIDError;
using rex::hid::Hub;
using rex::hid::IDeviceAssignment;
using rex::hid::kXInputGamepadA;
using rex::hid::kXInputGamepadB;
using rex::hid::SharedAssignment;
using rex::hid::SlotAssignment;
using rex::hid::X_INPUT_CAPABILITIES;
using rex::hid::X_INPUT_KEYSTROKE;
using rex::hid::X_INPUT_STATE;
using rex::hid::X_INPUT_VIBRATION;

namespace {

Device Pad(rex::u64 id, rex::u32 ordinal) {
  Device info;
  info.id = static_cast<DeviceId>(id);
  info.ordinal = ordinal;
  return info;
}

std::vector<DeviceId> GetDevicesForUser(const IDeviceAssignment& a, rex::u32 user) {
  std::vector<DeviceId> out;
  a.GetDevicesForUser(user, out);
  return out;
}

/** Pad driver with no SDL, window or OS behind it. */
class FakeDriver final : public Driver {
 public:
  void Add(rex::u64 id, bool synthetic = false) {
    Device info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    devices_.push_back(info);
    states_[static_cast<DeviceId>(id)] = {};
  }

  void Remove(rex::u64 id) {
    std::erase_if(devices_, [&](const Device& d) { return d.id == static_cast<DeviceId>(id); });
    states_.erase(static_cast<DeviceId>(id));
  }

  void SetButtons(rex::u64 id, rex::u16 buttons) {
    states_[static_cast<DeviceId>(id)].gamepad.buttons = buttons;
  }

  void QueueKeystroke(rex::u64 id, rex::u16 vk) {
    X_INPUT_KEYSTROKE ks = {};
    ks.virtual_key = vk;
    keystrokes_[static_cast<DeviceId>(id)] = ks;
  }

  void EnumerateDevices(std::vector<Device>& out) override { out = devices_; }

  [[nodiscard]] rex::Result<X_INPUT_STATE> GetDeviceState(DeviceId id) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return std::unexpected(Unplugged());
    }
    return it->second;
  }

  [[nodiscard]] rex::Result<X_INPUT_CAPABILITIES> GetDeviceCapabilities(DeviceId id,
                                                                        rex::u32) override {
    if (states_.find(id) == states_.end()) {
      return std::unexpected(Unplugged());
    }
    X_INPUT_CAPABILITIES caps{};
    caps.sub_type = static_cast<rex::u8>(id);
    return caps;
  }

  [[nodiscard]] rex::Result<void> SetDeviceVibration(DeviceId id,
                                                     const X_INPUT_VIBRATION&) override {
    if (!states_.contains(id)) {
      return std::unexpected(Unplugged());
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<X_INPUT_KEYSTROKE> GetDeviceKeystroke(DeviceId id, rex::u32) override {
    if (states_.find(id) == states_.end()) {
      return std::unexpected(Unplugged());
    }
    auto it = keystrokes_.find(id);
    if (it == keystrokes_.end()) {
      return std::unexpected(rex::hid::BuildError(HIDError::kEmpty, "empty"));
    }
    return it->second;
  }

 private:
  static rex::Error Unplugged() {
    return rex::hid::BuildError(HIDError::kNotConnected, "unplugged");
  }

  std::vector<Device> devices_;
  std::map<DeviceId, X_INPUT_STATE> states_;
  std::map<DeviceId, X_INPUT_KEYSTROKE> keystrokes_;
};

std::unique_ptr<Hub> MakeSharedHub(FakeDriver** out_driver) {
  auto driver = std::make_unique<FakeDriver>();
  *out_driver = driver.get();
  auto system = std::make_unique<Hub>();
  system->AddPadDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return system;
}

}  // namespace

REX_TEST_CASE("SharedAssignment puts every device on user 0", "[hid]") {
  SharedAssignment shared;
  shared.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});

  REQUIRE(GetDevicesForUser(shared, 0) ==
          std::vector<DeviceId>{static_cast<DeviceId>(10), static_cast<DeviceId>(11)});
  REQUIRE(GetDevicesForUser(shared, 1).empty());
}

REX_TEST_CASE("SlotAssignment does not renumber survivors when a pad is unplugged", "[hid]") {
  SlotAssignment slots;
  slots.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});
  slots.OnDevicesChanged({Pad(11, 1)});

  REQUIRE(GetDevicesForUser(slots, 0).empty());
  REQUIRE(GetDevicesForUser(slots, 1) == std::vector<DeviceId>{static_cast<DeviceId>(11)});
}

REX_TEST_CASE("Both controllers reach title user 0", "[hid]") {
  FakeDriver* driver = nullptr;
  auto system = MakeSharedHub(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, kXInputGamepadA);
  driver->SetButtons(2, kXInputGamepadB);

  auto state = system->GetState(0);
  REQUIRE(state);
  REQUIRE(static_cast<rex::u16>(state->gamepad.buttons) == (kXInputGamepadA | kXInputGamepadB));
}

REX_TEST_CASE("Unplugging 1 controller leaves the other working", "[hid]") {
  FakeDriver* driver = nullptr;
  auto system = MakeSharedHub(&driver);
  driver->Add(1);
  driver->Add(2);

  REQUIRE(system->GetState(0));

  driver->Remove(1);
  driver->SetButtons(2, kXInputGamepadB);
  auto state = system->GetState(0);
  REQUIRE(state);
  REQUIRE(static_cast<rex::u16>(state->gamepad.buttons) == kXInputGamepadB);
}

REX_TEST_CASE("Capabilities follow the controller in hand", "[hid]") {
  FakeDriver* driver = nullptr;
  auto system = MakeSharedHub(&driver);
  driver->Add(1);
  driver->Add(2);

  driver->SetButtons(2, kXInputGamepadB);
  REQUIRE(system->GetState(0));

  auto caps = system->GetCapabilities(0, 0);
  REQUIRE(caps);
  REQUIRE(caps->sub_type == 2);
}

REX_TEST_CASE("A keystroke from the second controller reaches the title", "[hid]") {
  FakeDriver* driver = nullptr;
  auto system = MakeSharedHub(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->QueueKeystroke(2, 0x5800);

  auto ks = system->GetKeystroke(0, 0);
  REQUIRE(ks);
  REQUIRE(static_cast<rex::u16>(ks->virtual_key) == 0x5800);
}

REX_TEST_CASE("A reconnected pad comes back to title user 0", "[hid]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<Hub>();
  system->AddPadDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  driver->Add(1);
  REQUIRE(system->GetState(0));

  driver->Remove(1);
  driver->Add(2);
  driver->SetButtons(2, kXInputGamepadB);
  auto state = system->GetState(0);
  REQUIRE(state);
  REQUIRE(static_cast<rex::u16>(state->gamepad.buttons) == kXInputGamepadB);
}

REX_TEST_CASE("A synthetic device does not push the first pad off title user 0", "[hid]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<Hub>();
  system->AddPadDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  // The idle pad is always present and enumerates before any pad connects.
  driver->Add(1, true);
  REQUIRE(system->GetState(0));

  driver->Add(2);
  driver->SetButtons(2, kXInputGamepadB);
  auto state = system->GetState(0);
  REQUIRE(state);
  REQUIRE(static_cast<rex::u16>(state->gamepad.buttons) == kXInputGamepadB);
  CHECK(CodeOf(system->GetState(1).error()) == HIDError::kNotConnected);
}
