/**
 * @file        tests/unit/apu/null_backend_test.cpp
 * @brief       apu::NullAPUBackend paces frames at the device rate
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
#include <thread>

#include <rex/apu.h>

namespace {

std::array<rex::be_f32, rex::apu::kFrameSamples> g_silence{};

}  // namespace

REX_TEST_CASE("NullAPUBackend plays 1 frame per period", "[apu]") {
  rex::apu::NullAPUBackend backend;
  std::atomic<rex::u32> played{0};
  auto stream = backend.OpenStream([&played] { played.fetch_add(1); });
  REQUIRE(stream);
  constexpr rex::u32 kQueued = 800;
  for (rex::u32 i = 0; i < kQueued; ++i) {
    backend.Submit(*stream, rex::apu::Frame(g_silence));
  }
  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  while (true) {
    const auto count = static_cast<rex::f64>(played.load());
    const auto elapsed = std::chrono::duration<rex::f64>(std::chrono::steady_clock::now() - start);
    const rex::f64 expected =
        elapsed.count() * rex::apu::kFrameRate / static_cast<rex::f64>(rex::apu::kChannelSamples);
    INFO("played " << count << ", expected " << expected);
    REQUIRE(count <= expected + 4.0);
    if (count >= expected - 4.0) {
      break;
    }
    REQUIRE(elapsed.count() < 3.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  backend.CloseStream(*stream);
}

REX_TEST_CASE("NullAPUBackend calls back only for submitted frames", "[apu]") {
  rex::apu::NullAPUBackend backend;
  std::atomic<rex::u32> played{0};
  auto stream = backend.OpenStream([&played] { played.fetch_add(1); });
  REQUIRE(stream);
  backend.Submit(*stream, rex::apu::Frame(g_silence));
  backend.Submit(*stream, rex::apu::Frame(g_silence));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(played.load() == 2);
  backend.CloseStream(*stream);
}

REX_TEST_CASE("NullAPUBackend drops queued frames on CloseStream", "[apu]") {
  rex::apu::NullAPUBackend backend;
  std::atomic<rex::u32> played{0};
  auto stream = backend.OpenStream([&played] { played.fetch_add(1); });
  REQUIRE(stream);
  for (int i = 0; i < 100; ++i) {
    backend.Submit(*stream, rex::apu::Frame(g_silence));
  }
  backend.CloseStream(*stream);
  const rex::u32 after_close = played.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(played.load() == after_close);
}
