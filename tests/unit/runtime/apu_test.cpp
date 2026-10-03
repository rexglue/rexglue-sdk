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

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include <rex/apu.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>

#include "../kernel/kernel_fixture.h"
#include "runtime/apu.h"
#include "runtime/xma/decoder.h"

using rex::X_STATUS;

namespace {

class RecordingBackend final : public rex::apu::IBackend {
 public:
  rex::Result<rex::u32> OpenStream(std::move_only_function<void()> on_frame_played) override {
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
  rex::apu::StereoFold stereo_fold() const override { return {}; }
  void SetSurroundMix(const rex::apu::SurroundMix&) override {}
  rex::apu::SurroundMix surround_mix() const override { return {}; }
  void SetOutputGain(rex::f32) override {}
  rex::f32 output_gain() const override { return 1.0f; }

  void Play(rex::u32 stream) {
    std::lock_guard lock(mutex_);
    callbacks_.at(stream)();
  }
  size_t open() const {
    std::lock_guard lock(mutex_);
    return callbacks_.size();
  }
  int closed() const { return closed_; }

 private:
  mutable std::mutex mutex_;
  std::map<rex::u32, std::move_only_function<void()>> callbacks_;
  std::vector<rex::u32> submitted_;
  rex::u32 next_ = 1;
  int closed_ = 0;
};

}  // namespace

TEST_CASE("APU gives a client its queue depth in credits", "[apu]") {
  rex::testing::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.runtime(), backend, 4);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  CHECK(apu.credits(*client) == 4);
  CHECK(backend.open() == 1);
  apu.UnregisterClient(*client);
  CHECK(backend.closed() == 1);
  CHECK(apu.credits(*client) == 0);
  apu.Shutdown();
}

TEST_CASE("APU gets 1 credit back per played frame", "[apu]") {
  rex::testing::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.runtime(), backend, 4);
  auto client = apu.RegisterClient(0, 0);
  REQUIRE(client);
  REQUIRE(apu.TakeCredit(*client));
  REQUIRE(apu.TakeCredit(*client));
  CHECK(apu.credits(*client) == 2);
  backend.Play(1);
  CHECK(apu.credits(*client) == 3);
  apu.Shutdown();
}

TEST_CASE("APU refuses a 9th client with X_STATUS_NO_MEMORY", "[apu]") {
  rex::testing::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.runtime(), backend, 4);
  for (rex::u32 i = 0; i < rex::runtime::APU::kMaxClients; ++i) {
    REQUIRE(apu.RegisterClient(0, 0));
  }
  auto ninth = apu.RegisterClient(0, 0);
  REQUIRE_FALSE(ninth);
  CHECK(rex::runtime::ToXStatus(ninth.error()) == X_STATUS_NO_MEMORY);
  CHECK(backend.open() == rex::runtime::APU::kMaxClients);
  apu.Shutdown();
}

TEST_CASE("APU pause is idempotent and resume restores it", "[apu]") {
  rex::testing::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.runtime(), backend, 4);
  REQUIRE(apu.Setup());
  apu.SetPaused(true);
  apu.SetPaused(true);
  CHECK(apu.paused());
  CHECK(apu.xma_decoder().is_paused());
  apu.SetPaused(false);
  CHECK_FALSE(apu.paused());
  CHECK_FALSE(apu.xma_decoder().is_paused());
  apu.Shutdown();
}

TEST_CASE("APU shutdown releases a paused worker", "[apu]") {
  rex::testing::KernelFixture fixture;
  RecordingBackend backend;
  rex::runtime::APU apu(fixture.runtime(), backend, 4);
  REQUIRE(apu.Setup());
  apu.SetPaused(true);
  const auto start = std::chrono::steady_clock::now();
  apu.Shutdown();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
  CHECK_FALSE(apu.paused());
}
