/**
 * @file        tests/unit/ppc/mmio_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ppc/mmio.h>

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

rex::ppc::MMIORecipient RecipientFor(Recorder& recorder) {
  return {&recorder, RecorderRead, RecorderWrite};
}

}  // namespace

REX_TEST_CASE("MMIO block helpers match both blocks at every edge", "[ppc][mmio]") {
  STATIC_REQUIRE(rex::ppc::MMIOBlockBase(rex::ppc::MMIOBlock::kGPU) == 0x7FC80000u);
  STATIC_REQUIRE(rex::ppc::MMIOBlockBase(rex::ppc::MMIOBlock::kAPU) == 0x7FEA0000u);
  STATIC_REQUIRE_FALSE(rex::ppc::IsMMIOBlockAddress(0x7FC7FFFFu));
  STATIC_REQUIRE(rex::ppc::MMIOBlockOf(0x7FC80000u) == rex::ppc::MMIOBlock::kGPU);
  STATIC_REQUIRE(rex::ppc::MMIOBlockOf(0x7FC8FFFFu) == rex::ppc::MMIOBlock::kGPU);
  STATIC_REQUIRE_FALSE(rex::ppc::IsMMIOBlockAddress(0x7FC90000u));
  STATIC_REQUIRE_FALSE(rex::ppc::IsMMIOBlockAddress(0x7FE9FFFFu));
  STATIC_REQUIRE(rex::ppc::MMIOBlockOf(0x7FEA0000u) == rex::ppc::MMIOBlock::kAPU);
  STATIC_REQUIRE(rex::ppc::MMIOBlockOf(0x7FEAFFFFu) == rex::ppc::MMIOBlock::kAPU);
  STATIC_REQUIRE_FALSE(rex::ppc::IsMMIOBlockAddress(0x7FEB0000u));
  STATIC_REQUIRE_FALSE(rex::ppc::IsMMIOBlockAddress(0x00000000u));
  STATIC_REQUIRE_FALSE(rex::ppc::IsMMIOBlockAddress(0xFFFFFFFFu));
}

REX_TEST_CASE("An attached recipient serves its block until it detaches", "[ppc][mmio]") {
  Recorder gpu{.value = 0x11};
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kGPU, RecipientFor(gpu));
  CHECK(rex::ppc::MMIOLoad32(0x7FC86530) == 0x11u);
  CHECK(gpu.last_address == 0x7FC86530u);
  rex::ppc::MMIOStore32(0x7FC80010, 0x22);
  CHECK(gpu.last_address == 0x7FC80010u);
  CHECK(gpu.last_value == 0x22u);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kGPU, &gpu);
  CHECK(rex::ppc::MMIOLoad32(0x7FC86530) == 0u);
}

REX_TEST_CASE("A late detach by an old owner keeps the new owner", "[ppc][mmio]") {
  Recorder old_owner{.value = 1};
  Recorder new_owner{.value = 2};
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kAPU, RecipientFor(old_owner));
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kAPU, RecipientFor(new_owner));
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kAPU, &old_owner);
  CHECK(rex::ppc::MMIOLoad32(0x7FEA0000) == 2u);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kAPU, &new_owner);
  CHECK(rex::ppc::MMIOLoad32(0x7FEA0000) == 0u);
}

REX_TEST_CASE("The null recipient returns 0 and drops stores", "[ppc][mmio]") {
  rex::ppc::MMIOStore32(0x7FC80000, 0x33);
  CHECK(rex::ppc::MMIOLoad32(0x7FC80000) == 0u);
  rex::ppc::MMIOStore32(0x7FEA0000, 0x44);
  CHECK(rex::ppc::MMIOLoad32(0x7FEA0000) == 0u);
  CHECK(rex::ppc::detail::mmio_slots[0].context == nullptr);
  CHECK(rex::ppc::detail::mmio_slots[1].context == nullptr);
}

REX_TEST_CASE("Guarded accesses pick the slot by block", "[ppc][mmio]") {
  Recorder gpu{.value = 0xA};
  Recorder apu{.value = 0xB};
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kGPU, RecipientFor(gpu));
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kAPU, RecipientFor(apu));
  CHECK(rex::ppc::MMIOLoad32(0x7FC8FFFC) == 0xAu);
  CHECK(rex::ppc::MMIOLoad32(0x7FEA0004) == 0xBu);
  rex::ppc::MMIOStore32(0x7FEA0008, 5);
  CHECK(apu.last_value == 5u);
  CHECK(gpu.last_value == 0u);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kGPU, &gpu);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kAPU, &apu);
}

REX_TEST_CASE("An incomplete recipient is refused", "[ppc][mmio]") {
  Recorder recorder;
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kGPU, {&recorder, nullptr, RecorderWrite});
  CHECK(rex::ppc::detail::mmio_slots[0].context == nullptr);
  CHECK(rex::ppc::MMIOLoad32(0x7FC80000) == 0u);
}

REX_TEST_CASE("An address outside both blocks reaches no recipient", "[ppc][mmio]") {
  Recorder gpu{.value = 0xA};
  Recorder apu{.value = 0xB};
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kGPU, RecipientFor(gpu));
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kAPU, RecipientFor(apu));
  CHECK(rex::ppc::MMIOLoad32(0x7FEB0000) == 0u);
  rex::ppc::MMIOStore32(0x7FC90000, 7);
  CHECK(gpu.last_address == 0u);
  CHECK(apu.last_address == 0u);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kGPU, &gpu);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kAPU, &apu);
}

REX_TEST_CASE("Narrow guarded loads read the big endian lane of the register", "[ppc][mmio]") {
  Recorder gpu{.value = 0x11223344};
  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kGPU, RecipientFor(gpu));
  CHECK(rex::ppc::MMIOLoad8(0x7FC80010) == 0x11u);
  CHECK(rex::ppc::MMIOLoad8(0x7FC80011) == 0x22u);
  CHECK(rex::ppc::MMIOLoad8(0x7FC80012) == 0x33u);
  CHECK(rex::ppc::MMIOLoad8(0x7FC80013) == 0x44u);
  CHECK(gpu.last_address == 0x7FC80010u);
  CHECK(rex::ppc::MMIOLoad16(0x7FC80010) == 0x1122u);
  CHECK(rex::ppc::MMIOLoad16(0x7FC80012) == 0x3344u);
  CHECK(gpu.last_address == 0x7FC80010u);
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kGPU, &gpu);
}
