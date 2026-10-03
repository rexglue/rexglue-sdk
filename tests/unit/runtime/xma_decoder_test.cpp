/**
 * @file        tests/unit/runtime/xma_decoder_test.cpp
 * @brief       XMA decoder worker lifetime
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "runtime/xma/decoder.h"
#include <rex/mmio.h>
#include <rex/runtime.h>

#include "../kernel/kernel_fixture.h"

using rex::X_STATUS;

TEST_CASE("An XMA decoder destroyed without Shutdown stops its worker", "[apu][xma]") {
  rex::testing::KernelFixture fixture;
  auto decoder =
      std::make_unique<rex::runtime::XMADecoder>(fixture.runtime().function_dispatcher());
  REQUIRE(decoder->Setup() == X_STATUS_SUCCESS);
  std::promise<void> destroyed;
  std::future<void> done = destroyed.get_future();
  std::thread([raw = decoder.release(), signal = std::move(destroyed)]() mutable {
    delete raw;
    signal.set_value();
  }).detach();
  CHECK(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

TEST_CASE("An XMA decoder serves the APU block until it is destroyed", "[apu][xma][mmio]") {
  rex::testing::KernelFixture fixture;
  auto decoder =
      std::make_unique<rex::runtime::XMADecoder>(fixture.runtime().function_dispatcher());
  REQUIRE(decoder->Setup() == X_STATUS_SUCCESS);
  CHECK(rex::detail::mmio_slots[1].context == decoder.get());
  decoder.reset();
  CHECK(rex::detail::mmio_slots[1].context == nullptr);
}
