/**
 * @file        tests/unit/runtime/apu_test.cpp
 * @brief       runtime::APU client credits, pause and the client limit
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <map>
#include <mutex>
#include <vector>

#include <rex/apu.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/move_only_function.h>

#include "../kernel/kernel_fixture.h"
#include "runtime/apu.h"
#include "runtime/xma/decoder.h"

using rex::X_STATUS;

namespace {

class RecordingBackend final : public rex::apu::IAPUBackend {
 public:
  [[nodiscard]] rex::Result<rex::u32> OpenStream(
      rex::MoveOnlyFunction<void()> on_frame_played) override {
    std::lock_guard lock(mutex_);
    const rex::u32 id = next_++;
    callbacks_.emplace(id, std::move(on_frame_played));
    return id;
  }
  void CloseStream(rex::u32 stream) override {
    std::lock_guard lock(mutex_);
    callbacks_.erase(stream);
    ++closed_;
  }
  void Submit(rex::u32 stream, rex::apu::Frame) override {
    std::lock_guard lock(mutex_);
    submitted_.push_back(stream);
  }
  void SetStereoFold(const rex::apu::StereoFold&) override {}
  rex::apu::StereoFold GetStereoFold() const override { return {}; }
  void SetSurroundMix(const rex::apu::SurroundMix&) override {}
  rex::apu::SurroundMix GetSurroundMix() const override { return {}; }
  void SetOutputGain(rex::f32) override {}
  rex::f32 GetOutputGain() const override { return 1.0f; }

  void Play(rex::u32 stream) {
    std::lock_guard lock(mutex_);
    callbacks_.at(stream)();
  }
  size_t GetOpenCount() const {
    std::lock_guard lock(mutex_);
    return callbacks_.size();
  }
  int GetClosed() const { return closed_; }

 private:
  mutable std::mutex mutex_;
  std::map<rex::u32, rex::MoveOnlyFunction<void()>> callbacks_;
  std::vector<rex::u32> submitted_;
  rex::u32 next_ = 1;
  int closed_ = 0;
};

}  // namespace

REX_TEST_CASE("APU gives a client its queue depth in credits", "[runtime]") {
  rex::test::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.GetRuntime(), backend, 4);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  CHECK(apu.CountCredits(*client) == 4);
  CHECK(backend.GetOpenCount() == 1);
  apu.UnregisterClient(*client);
  CHECK(backend.GetClosed() == 1);
  CHECK(apu.CountCredits(*client) == 0);
  apu.Shutdown();
}

REX_TEST_CASE("APU gets 1 credit back per played frame", "[runtime]") {
  rex::test::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.GetRuntime(), backend, 4);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  REQUIRE(apu.TakeCredit(*client));
  REQUIRE(apu.TakeCredit(*client));
  CHECK(apu.CountCredits(*client) == 2);
  backend.Play(1);
  CHECK(apu.CountCredits(*client) == 3);
  apu.Shutdown();
}

REX_TEST_CASE("APU refuses a 9th client with X_STATUS_NO_MEMORY", "[runtime]") {
  rex::test::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.GetRuntime(), backend, 4);
  for (rex::u32 i = 0; i < rex::runtime::APU::kMaxClients; ++i) {
    REQUIRE(apu.RegisterClient(0, 0));
  }
  auto ninth = apu.RegisterClient(0, 0);
  REQUIRE_FALSE(ninth);
  CHECK(rex::runtime::ToXStatus(ninth.error()) == rex::X_STATUS_NO_MEMORY);
  CHECK(backend.GetOpenCount() == rex::runtime::APU::kMaxClients);
  apu.Shutdown();
}

REX_TEST_CASE("APU pause is idempotent and resume restores it", "[runtime]") {
  rex::test::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.GetRuntime(), backend, 4);
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

REX_TEST_CASE("APU shutdown releases a paused worker", "[runtime]") {
  rex::test::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.GetRuntime(), backend, 4);
  REQUIRE(apu.Setup());
  apu.SetPaused(true);
  const auto start = std::chrono::steady_clock::now();
  apu.Shutdown();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
  CHECK_FALSE(apu.IsPaused());
}

namespace {

std::atomic<rex::u32> g_callback_event{0};
std::atomic<bool> g_in_callback{false};

void WaitingClientCallback(PPCContext& ctx, rex::u8* base) {
  g_in_callback.store(true);
  rex::test::Caller caller(ctx, base, *rex::runtime::ThreadState::Get()->GetRuntime());
  caller.Kernel("NtWaitForSingleObjectEx", {g_callback_event.load(), 1, 0, 0});
}

}  // namespace

REX_TEST_CASE("APU shutdown stops a worker blocked in its title callback", "[runtime][shutdown]") {
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
  const rex::u32 callback =
      fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&WaitingClientCallback, 0);
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.GetRuntime(), backend, 4);
  REQUIRE(apu.Setup());
  REQUIRE(apu.RegisterClient(callback, 0));
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!g_in_callback.load() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(g_in_callback.load());
  const auto start = std::chrono::steady_clock::now();
  apu.Shutdown();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
}
