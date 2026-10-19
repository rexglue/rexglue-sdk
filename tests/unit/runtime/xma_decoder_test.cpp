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

#include "rex_test.h"

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "runtime/xma/decoder.h"
#include <rex/mmio.h>
#include <rex/runtime.h>

#include "../kernel/kernel_fixture.h"

using rex::X_STATUS;

REX_TEST_CASE("An XMA decoder destroyed without Shutdown stops its worker", "[runtime][xma]") {
  rex::test::KernelFixture fixture;
  auto decoder =
      std::make_unique<rex::runtime::XMADecoder>(fixture.GetRuntime().GetFunctionDispatcher());
  REQUIRE(decoder->Setup() == rex::X_STATUS_SUCCESS);
  std::promise<void> destroyed;
  std::future<void> done = destroyed.get_future();
  std::thread([owned = std::move(decoder), signal = std::move(destroyed)]() mutable {
    owned.reset();
    signal.set_value();
  }).detach();
  CHECK(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

REX_TEST_CASE("An XMA decoder serves the APU block until it is destroyed", "[runtime][xma][mmio]") {
  rex::test::KernelFixture fixture;
  auto decoder =
      std::make_unique<rex::runtime::XMADecoder>(fixture.GetRuntime().GetFunctionDispatcher());
  REQUIRE(decoder->Setup() == rex::X_STATUS_SUCCESS);
  CHECK(rex::detail::mmio_slots[1].context == decoder.get());
  decoder.reset();
  CHECK(rex::detail::mmio_slots[1].context == nullptr);
}

namespace {

std::unique_ptr<rex::runtime::XMADecoder> MakeDecoder(rex::test::KernelFixture& fixture) {
  auto decoder =
      std::make_unique<rex::runtime::XMADecoder>(fixture.GetRuntime().GetFunctionDispatcher());
  REQUIRE(decoder->Setup() == rex::X_STATUS_SUCCESS);
  return decoder;
}

}  // namespace

REX_TEST_CASE("Releasing an XMA context twice leaves the released memory alone", "[runtime][xma]") {
  rex::test::KernelFixture fixture;
  auto decoder = MakeDecoder(fixture);
  const rex::u32 context = decoder->AllocateContext();
  REQUIRE(context != 0);
  auto* memory = fixture.GetRuntime().GetMemory();
  decoder->ReleaseContext(context);
  *memory->TranslateVirtual<rex::u8*>(context) = 0x5A;
  decoder->ReleaseContext(context);
  CHECK(*memory->TranslateVirtual<rex::u8*>(context) == 0x5A);
}

REX_TEST_CASE("An XMA context call with a pointer outside the context array does nothing",
              "[runtime][xma]") {
  rex::test::KernelFixture fixture;
  auto decoder = MakeDecoder(fixture);
  decoder->ReleaseContext(0x1234);
  CHECK(decoder->BlockOnContext(0x1234, true));
  CHECK(decoder->BlockOnContext(0xFFFFFFFF, false));
}

REX_TEST_CASE("Signaling an XMA context outside the context array writes no register",
              "[runtime][xma]") {
  rex::test::KernelFixture fixture;
  auto decoder = MakeDecoder(fixture);
  constexpr rex::u32 kEnableBase = 0x1940;
  const rex::u32 array = decoder->GetContextArrayAddress();
  std::array<rex::u32, 10> before{};
  for (rex::u32 i = 0; i < before.size(); ++i) {
    before[i] = decoder->ReadRegister(kEnableBase + i * 4);
  }
  CHECK_FALSE(decoder->SignalContext(kEnableBase, UINT32_MAX));
  CHECK_FALSE(decoder->SignalContext(kEnableBase, array - 64));
  CHECK_FALSE(decoder->SignalContext(kEnableBase, array + 320 * 64));
  for (rex::u32 i = 0; i < before.size(); ++i) {
    CHECK(decoder->ReadRegister(kEnableBase + i * 4) == before[i]);
  }
  CHECK(decoder->SignalContext(0x1A40, array + 5 * 64));
}
