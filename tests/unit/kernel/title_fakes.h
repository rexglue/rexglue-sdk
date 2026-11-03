/**
 * @file        tests/unit/kernel/title_fakes.h
 * @brief       Counting fake drivers for every driver type and the helpers that wait on a title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/apu/audio_driver.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/kernel/title_runtime.h>

#include "ui/ui_harness.h"

namespace rex::test {

struct Counts {
  std::atomic<int> graphics_attach{0};
  std::atomic<int> graphics_detach{0};

  void Reset() { graphics_attach = graphics_detach = 0; }
};

inline Counts g_counts;
inline std::vector<rex::hid::PadDevice> g_pads;
inline std::atomic<rex::u64> g_pad_revision{1};

inline rex::hid::PadDevice MakePad(rex::u64 id, rex::i32 xinput_slot) {
  rex::hid::PadDevice device;
  device.id = static_cast<rex::hid::PadDeviceID>(id);
  device.name = "pad";
  device.identity.xinput_slot = xinput_slot;
  return device;
}

class CountingInput final : public rex::hid::IInputDriver {
 public:
  std::string_view GetName() const override { return "count"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  rex::u64 GetDeviceRevision() const override { return g_pad_revision; }
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override { out = g_pads; }
  rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID) override {
    return rex::hid::PadState{};
  }
  rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID) override {
    rex::hid::PadCapabilities caps;
    caps.supported.buttons = 0xFFFF;
    return caps;
  }
  rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID,
                                       const rex::hid::PadVibration&) override {
    return {};
  }
};

class CountingAudio final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return "count"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  rex::Result<rex::u32> OpenDevice(rex::u32 wanted, rex::apu::AudioFill) override { return wanted; }
  void CloseDevice() override {}
  bool IsDeviceLost() const override { return false; }
};

class CountingGraphics final : public rex::gpu::IXenosGraphicsDriver {
 public:
  std::string_view GetName() const override { return "count"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  rex::Result<void> AttachTitle(const rex::TitleInfo&, rex::IPhysicalMemory&,
                                rex::IPhysicalWriteWatch&, rex::IGuestInterrupts&) override {
    ++g_counts.graphics_attach;
    return {};
  }
  void DetachTitle() override { ++g_counts.graphics_detach; }
  rex::gpu::FrameStatus WaitForFrame(rex::u32) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return {};
  }
  void ResizeSurface(rex::u32, rex::u32) override {}
  bool BeginPresent() override { return false; }
  void EndPresent() override {}
  void RenderOverlays(ImDrawData&) override {}
  void InitializeRingBuffer(rex::u32, rex::u32) override {}
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override {}
  rex::u32 ReadRegister(rex::u32) override { return 0; }
  void WriteRegister(rex::u32, rex::u32) override {}
};

inline std::unique_ptr<rex::IDriver> CreateUI() {
  return std::make_unique<rex::test_ui::FakeUIDriver>();
}
inline std::unique_ptr<rex::IDriver> CreateInput() {
  return std::make_unique<CountingInput>();
}
inline std::unique_ptr<rex::IDriver> CreateAudio() {
  return std::make_unique<CountingAudio>();
}
inline std::unique_ptr<rex::IDriver> CreateGraphics() {
  return std::make_unique<CountingGraphics>();
}

class CountingProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "count"; }
  rex::Result<void> Connect(rex::IFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 4> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kUI, "count", {}, &CreateUI},
      rex::DriverRegistration{rex::DriverType::kInput, "count", {}, &CreateInput},
      rex::DriverRegistration{rex::DriverType::kAudio, "count", {}, &CreateAudio},
      rex::DriverRegistration{rex::DriverType::kGraphics, "count", rex::GraphicsFamily::kXenos,
                              &CreateGraphics}};
};

inline bool WaitFor(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

inline void UnloadWithin(rex::kernel::TitleRuntime& title, std::chrono::seconds limit) {
  std::jthread watchdog([limit](const std::stop_token& stop) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    if (!wake.wait_for(lock, stop, limit, [] { return false; }) && !stop.stop_requested()) {
      std::fputs("Unload did not return in time\n", stderr);
      std::abort();
    }
  });
  title.Unload();
  watchdog.request_stop();
}

}  // namespace rex::test
