/**
 * @file        tests/unit/kernel/apu_test.cpp
 * @brief       kernel::APU client credits, guest frames, pause and the client limit
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <rex/apu/audio_driver.h>
#include <rex/apu/audio_system.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>

#include "../kernel/kernel_fixture.h"
#include "pull_device.h"
#include "kernel/apu.h"
#include "kernel/xma/decoder.h"
#include "kernel/title_state.h"

using rex::kernel::X_STATUS;

namespace {

constexpr rex::u32 kFrameBytes = rex::apu::kAudioFrameSamples * sizeof(rex::f32);

struct CaptureData {
  std::mutex mutex;
  std::vector<std::vector<rex::f32>> blocks;

  std::vector<std::vector<rex::f32>> Take() {
    std::lock_guard lock(mutex);
    return std::exchange(blocks, {});
  }
};

CaptureData g_capture;

class CaptureDriver final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return "capture"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  rex::Result<rex::u32> OpenDevice(rex::u32 channels, rex::apu::AudioFill fill) override {
    device_.Open(std::move(fill), channels, [](std::span<const rex::f32> samples) {
      std::lock_guard lock(g_capture.mutex);
      g_capture.blocks.emplace_back(samples.begin(), samples.end());
    });
    return channels;
  }
  void CloseDevice() override { device_.Close(); }
  bool IsDeviceLost() const override { return false; }

 private:
  rex::test::PullDevice device_;
};

std::unique_ptr<rex::IDriver> CreateCapture() {
  return std::make_unique<CaptureDriver>();
}

class CaptureProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "capture"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 1> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kAudio, "capture", {}, &CreateCapture}};
};

bool WaitFor(const std::function<bool()>& condition, int milliseconds = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return condition();
}

rex::u32 AllocateFrame(rex::kernel::TitleRuntime& runtime) {
  return StateOf(runtime).GetAddressSpace()->SystemHeapAlloc(kFrameBytes);
}

}  // namespace

REX_TEST_CASE("APU gives a client its queue depth in credits", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  CHECK(apu.CountCredits(*client) == audio->GetSettings().max_queued_frames.Get());
  CHECK(audio->GetStats().open_streams == 1);
  apu.UnregisterClient(*client);
  CHECK(audio->GetStats().open_streams == 0);
  CHECK(apu.CountCredits(*client) == 0);
  apu.Shutdown();
}

REX_TEST_CASE("APU gets 1 credit back per played frame", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  const rex::u32 full = audio->GetSettings().max_queued_frames.Get();
  REQUIRE(apu.TakeCredit(*client));
  REQUIRE(apu.TakeCredit(*client));
  CHECK(apu.CountCredits(*client) == full - 2);
  const rex::u32 frame = AllocateFrame(fixture.GetRuntime());
  apu.SubmitFrame(*client, frame);
  apu.SubmitFrame(*client, frame);
  CHECK(WaitFor([&] { return apu.CountCredits(*client) == full; }));
  apu.Shutdown();
}

REX_TEST_CASE("guest frame reaches the mix in host order", "[kernel]") {
  CaptureProvider provider;
  g_capture.Take();
  rex::test::KernelFixture fixture({}, {&provider}, "capture");
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  const rex::u32 frame = AllocateFrame(fixture.GetRuntime());
  auto* bytes = StateOf(fixture.GetRuntime()).GetAddressSpace()->TranslateVirtual<rex::u8*>(frame);
  std::memset(bytes, 0, kFrameBytes);
  bytes[0] = 0x3F;
  apu.SubmitFrame(*client, frame);
  bool found = false;
  CHECK(WaitFor([&] {
    for (const auto& block : g_capture.Take()) {
      if (block.size() >= 6 && block[0] > 0.49f && block[0] < 0.51f) {
        found = true;
      }
    }
    return found;
  }));
  apu.Shutdown();
}

REX_TEST_CASE("a frame for an unknown client is ignored", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  apu.SubmitFrame(7, AllocateFrame(fixture.GetRuntime()));
  CHECK(audio->GetStats().queued_frames == 0);
  apu.Shutdown();
}

REX_TEST_CASE("APU refuses a 9th client with X_STATUS_NO_MEMORY", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  for (rex::u32 i = 0; i < rex::kernel::APU::kMaxClients; ++i) {
    REQUIRE(apu.RegisterClient(0, 0));
  }
  auto ninth = apu.RegisterClient(0, 0);
  REQUIRE_FALSE(ninth);
  CHECK(rex::kernel::ToXStatus(ninth.error()) == rex::kernel::X_STATUS_NO_MEMORY);
  CHECK(audio->GetStats().open_streams == rex::kernel::APU::kMaxClients);
  apu.Shutdown();
}

REX_TEST_CASE("APU pause is idempotent and resume restores it", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  REQUIRE(apu.Setup());
  apu.SetPaused(true);
  apu.SetPaused(true);
  CHECK(apu.IsPaused());
  CHECK(apu.GetXMADecoder().IsPaused());
  apu.SetPaused(false);
  CHECK_FALSE(apu.IsPaused());
  CHECK_FALSE(apu.GetXMADecoder().IsPaused());
  apu.Shutdown();
}

REX_TEST_CASE("APU shutdown releases a paused worker", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  REQUIRE(apu.Setup());
  apu.SetPaused(true);
  const auto start = std::chrono::steady_clock::now();
  apu.Shutdown();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
  CHECK_FALSE(apu.IsPaused());
}

namespace {

std::atomic<rex::u32> g_callback_event{0};
std::atomic<bool> g_in_callback{false};

void WaitingClientCallback(PPCContext& ctx, rex::u8* base) {
  g_in_callback.store(true);
  rex::test::Caller caller(ctx, base, *rex::kernel::ThreadState::Get()->GetRuntime());
  caller.Kernel("NtWaitForSingleObjectEx", {g_callback_event.load(), 1, 0, 0});
}

}  // namespace

REX_TEST_CASE("APU shutdown stops a worker blocked in its title callback", "[kernel][shutdown]") {
  rex::test::KernelFixture fixture;
  g_in_callback.store(false);
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    const rex::u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    g_callback_event.store(c.Load32(handle_ptr));
  });
  const rex::u32 callback = StateOf(fixture.GetRuntime())
                                .GetFunctionDispatcher()
                                ->AllocateThunk(&WaitingClientCallback, 0);
  rex::apu::IAudioSystem* audio = &fixture.GetAudio();
  rex::kernel::APU apu(fixture.GetRuntime(), audio);
  REQUIRE(apu.Setup());
  REQUIRE(apu.RegisterClient(callback, 0));
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!g_in_callback.load() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(g_in_callback.load());
  const auto start = std::chrono::steady_clock::now();
  apu.Shutdown();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}
