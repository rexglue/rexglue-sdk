/**
 * @file        tests/unit/ppc/hook_shapes_test.cpp
 * @brief       The argument shapes the existing hooks use keep their placement
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ppc/function.h>
#include <rex/types.h>

using namespace rex::ppc;

REX_TEST_CASE("Integers, pointers and be_ values take one register each", "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<u32, u32, void*, rex::be_u32>();
  STATIC_REQUIRE(places[0].gpr == 0);
  STATIC_REQUIRE(places[1].gpr == 1);
  STATIC_REQUIRE(places[2].gpr == 2);
  STATIC_REQUIRE(places[3].gpr == 3);
  STATIC_REQUIRE(places[3].slot == 3);
  STATIC_REQUIRE(places[3].slots == 1);
  STATIC_REQUIRE_FALSE(places[3].stack);
  STATIC_REQUIRE(places[3].fpr == -1);
  STATIC_REQUIRE(places[3].vr == -1);
}

REX_TEST_CASE("A float takes the next float register and keeps its slot", "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<u32, f64, u32, u32, f64, u32>();
  STATIC_REQUIRE(places[1].fpr == 0);
  STATIC_REQUIRE(places[1].gpr == -1);
  STATIC_REQUIRE(places[1].slot == 1);
  STATIC_REQUIRE(places[2].gpr == 2);
  STATIC_REQUIRE(places[4].fpr == 1);
  STATIC_REQUIRE(places[4].slot == 4);
  STATIC_REQUIRE(places[5].gpr == 5);
}

REX_TEST_CASE("The 9th integer argument sits in the first stack slot", "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<u32, u32, u32, u32, u32, u32, u32, u32, u32, u32>();
  STATIC_REQUIRE(places[7].gpr == 7);
  STATIC_REQUIRE_FALSE(places[7].stack);
  STATIC_REQUIRE(places[8].gpr == -1);
  STATIC_REQUIRE(places[8].slot == 8);
  STATIC_REQUIRE(places[8].stack);
  STATIC_REQUIRE(places[9].slot == 9);
}

REX_TEST_CASE("The 14th float goes to the stack and the 13th stays in f13", "[ppc][hook_shapes]") {
  constexpr auto places =
      PlaceArgs<f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64>();
  STATIC_REQUIRE(places[12].fpr == 12);
  STATIC_REQUIRE(places[12].stack);
  STATIC_REQUIRE(places[13].fpr == -1);
  STATIC_REQUIRE(places[13].stack);
  STATIC_REQUIRE(places[13].slot == 13);
}

REX_TEST_CASE("A float past the 8th slot is in its register and its stack slot",
              "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<u32, u32, u32, u32, u32, u32, u32, u32, f64, f32>();
  STATIC_REQUIRE(places[8].fpr == 0);
  STATIC_REQUIRE(places[8].stack);
  STATIC_REQUIRE(places[9].fpr == 1);
  STATIC_REQUIRE(places[9].stack);
}

REX_TEST_CASE("A vector takes a vector register and no integer slot", "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<PPCVRegister, u32, u32>();
  STATIC_REQUIRE(places[0].vr == 0);
  STATIC_REQUIRE(places[0].gpr == -1);
  STATIC_REQUIRE(places[0].slots == 0);
  STATIC_REQUIRE(places[1].gpr == 0);
  STATIC_REQUIRE(places[2].gpr == 1);
}

REX_TEST_CASE("The 14th vector takes 2 stack slots at an even slot", "[ppc][hook_shapes]") {
  constexpr auto places =
      PlaceArgs<PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister,
                PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister,
                PPCVRegister, PPCVRegister>();
  STATIC_REQUIRE(places[12].vr == 12);
  STATIC_REQUIRE_FALSE(places[12].stack);
  STATIC_REQUIRE(places[13].vr == -1);
  STATIC_REQUIRE(places[13].stack);
  STATIC_REQUIRE(places[13].slot == 8);
  STATIC_REQUIRE(places[13].slots == 2);
}

namespace {

struct Twelve {
  rex::be_u32 a;
  rex::be_u32 b;
  rex::be_u32 c;
};

}  // namespace

REX_TEST_CASE("A 12 byte structure at slot 7 splits between r10 and the stack",
              "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<u32, u32, u32, u32, u32, u32, u32, ByValue<Twelve>>();
  STATIC_REQUIRE(places[7].slot == 7);
  STATIC_REQUIRE(places[7].slots == 2);
  STATIC_REQUIRE(places[7].gpr == 7);
  STATIC_REQUIRE(places[7].stack);
}

REX_TEST_CASE("A structure that fits the registers is not on the stack", "[ppc][hook_shapes]") {
  constexpr auto places = PlaceArgs<ByValue<Twelve>, u32>();
  STATIC_REQUIRE(places[0].slots == 2);
  STATIC_REQUIRE_FALSE(places[0].stack);
  STATIC_REQUIRE(places[1].gpr == 2);
}

REX_TEST_CASE("A hidden return pointer shifts the arguments of a signature", "[ppc][hook_shapes]") {
  using Returned = ByValue<Twelve>;
  constexpr auto places = PlaceFunctionArgs<+[](u32) -> Returned { return {}; }>();
  STATIC_REQUIRE(places[0].gpr == 1);
  STATIC_REQUIRE(HiddenReturnType<Returned>);
  STATIC_REQUIRE_FALSE(HiddenReturnType<ByValue<rex::be_u32>>);
  STATIC_REQUIRE(HiddenReturnType<ByValue<PPCVRegister>>);
}
