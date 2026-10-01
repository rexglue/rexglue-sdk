/**
 * @file        tests/unit/core/mmio_test.cpp
 * @brief       MMIO block helpers and the recipient table
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/mmio.h>

namespace {

struct Recorder {
  rex::u32 value = 0;
  rex::u32 last_address = 0;
  rex::u32 last_value = 0;
};

rex::u32 RecorderRead(void* context, rex::u32 address) {
  auto* recorder = static_cast<Recorder*>(context);
  recorder->last_address = address;
  return recorder->value;
}

void RecorderWrite(void* context, rex::u32 address, rex::u32 value) {
  auto* recorder = static_cast<Recorder*>(context);
  recorder->last_address = address;
  recorder->last_value = value;
}

rex::MMIORecipient RecipientFor(Recorder& recorder) {
  return {&recorder, RecorderRead, RecorderWrite};
}

}  // namespace

TEST_CASE("MMIO block helpers match both blocks at every edge", "[mmio]") {
  STATIC_REQUIRE(rex::MMIOBlockBase(rex::MMIOBlock::GPU) == 0x7FC80000u);
  STATIC_REQUIRE(rex::MMIOBlockBase(rex::MMIOBlock::APU) == 0x7FEA0000u);
  STATIC_REQUIRE_FALSE(rex::IsMMIOBlockAddress(0x7FC7FFFFu));
  STATIC_REQUIRE(rex::MMIOBlockOf(0x7FC80000u) == rex::MMIOBlock::GPU);
  STATIC_REQUIRE(rex::MMIOBlockOf(0x7FC8FFFFu) == rex::MMIOBlock::GPU);
  STATIC_REQUIRE_FALSE(rex::IsMMIOBlockAddress(0x7FC90000u));
  STATIC_REQUIRE_FALSE(rex::IsMMIOBlockAddress(0x7FE9FFFFu));
  STATIC_REQUIRE(rex::MMIOBlockOf(0x7FEA0000u) == rex::MMIOBlock::APU);
  STATIC_REQUIRE(rex::MMIOBlockOf(0x7FEAFFFFu) == rex::MMIOBlock::APU);
  STATIC_REQUIRE_FALSE(rex::IsMMIOBlockAddress(0x7FEB0000u));
  STATIC_REQUIRE_FALSE(rex::IsMMIOBlockAddress(0x00000000u));
  STATIC_REQUIRE_FALSE(rex::IsMMIOBlockAddress(0xFFFFFFFFu));
}

TEST_CASE("An attached recipient serves its block until it detaches", "[mmio]") {
  Recorder gpu{.value = 0x11};
  rex::AttachMMIO(rex::MMIOBlock::GPU, RecipientFor(gpu));
  CHECK(rex::detail::MMIOLoad(0x7FC86530) == 0x11u);
  CHECK(gpu.last_address == 0x7FC86530u);
  rex::detail::MMIOStore(0x7FC80010, 0x22);
  CHECK(gpu.last_address == 0x7FC80010u);
  CHECK(gpu.last_value == 0x22u);
  rex::DetachMMIO(rex::MMIOBlock::GPU, &gpu);
  CHECK(rex::detail::MMIOLoad(0x7FC86530) == 0u);
}

TEST_CASE("A late detach by an old owner keeps the new owner", "[mmio]") {
  Recorder old_owner{.value = 1};
  Recorder new_owner{.value = 2};
  rex::AttachMMIO(rex::MMIOBlock::APU, RecipientFor(old_owner));
  rex::AttachMMIO(rex::MMIOBlock::APU, RecipientFor(new_owner));
  rex::DetachMMIO(rex::MMIOBlock::APU, &old_owner);
  CHECK(rex::detail::MMIOLoad(0x7FEA0000) == 2u);
  rex::DetachMMIO(rex::MMIOBlock::APU, &new_owner);
  CHECK(rex::detail::MMIOLoad(0x7FEA0000) == 0u);
}

TEST_CASE("The null recipient returns 0 and drops stores", "[mmio]") {
  rex::detail::MMIOStore(0x7FC80000, 0x33);
  CHECK(rex::detail::MMIOLoad(0x7FC80000) == 0u);
  rex::detail::MMIOStore(0x7FEA0000, 0x44);
  CHECK(rex::detail::MMIOLoad(0x7FEA0000) == 0u);
  CHECK(rex::detail::mmio_slots[0].context == nullptr);
  CHECK(rex::detail::mmio_slots[1].context == nullptr);
}

TEST_CASE("Guarded accesses pick the slot by block", "[mmio]") {
  Recorder gpu{.value = 0xA};
  Recorder apu{.value = 0xB};
  rex::AttachMMIO(rex::MMIOBlock::GPU, RecipientFor(gpu));
  rex::AttachMMIO(rex::MMIOBlock::APU, RecipientFor(apu));
  CHECK(rex::detail::MMIOLoad(0x7FC8FFFC) == 0xAu);
  CHECK(rex::detail::MMIOLoad(0x7FEA0004) == 0xBu);
  rex::detail::MMIOStore(0x7FEA0008, 5);
  CHECK(apu.last_value == 5u);
  CHECK(gpu.last_value == 0u);
  rex::DetachMMIO(rex::MMIOBlock::GPU, &gpu);
  rex::DetachMMIO(rex::MMIOBlock::APU, &apu);
}

TEST_CASE("An incomplete recipient is refused", "[mmio]") {
  Recorder recorder;
  rex::AttachMMIO(rex::MMIOBlock::GPU, {&recorder, nullptr, RecorderWrite});
  CHECK(rex::detail::mmio_slots[0].context == nullptr);
  CHECK(rex::detail::MMIOLoad(0x7FC80000) == 0u);
}

TEST_CASE("An address outside both blocks reaches no recipient", "[mmio]") {
  Recorder gpu{.value = 0xA};
  Recorder apu{.value = 0xB};
  rex::AttachMMIO(rex::MMIOBlock::GPU, RecipientFor(gpu));
  rex::AttachMMIO(rex::MMIOBlock::APU, RecipientFor(apu));
  CHECK(rex::detail::MMIOLoad(0x7FEB0000) == 0u);
  rex::detail::MMIOStore(0x7FC90000, 7);
  CHECK(gpu.last_address == 0u);
  CHECK(apu.last_address == 0u);
  rex::DetachMMIO(rex::MMIOBlock::GPU, &gpu);
  rex::DetachMMIO(rex::MMIOBlock::APU, &apu);
}
