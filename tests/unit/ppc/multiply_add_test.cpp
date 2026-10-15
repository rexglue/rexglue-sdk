/**
 * @file        tests/unit/ppc/multiply_add_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstddef>
#include <span>

#include "rex_test.h"

#include <rex/ppc/multiply_add.h>

#include "multiply_add_cases.h"
#include "multiply_add_nj0_cases.h"

namespace {

using MultiplyAddFunction = simde__m128 (*)(simde__m128, simde__m128, simde__m128);

/**
 * Checks the lane results of a vector multiply add, with the cases of each vector taken from the
 * cases at the given stride.
 *
 * @tparam MultiplyAdd The vector multiply add.
 * @param cases The operands and expected result of each lane.
 * @param stride The distance between the cases that share a vector.
 */
template <MultiplyAddFunction MultiplyAdd>
void CheckLanes(std::span<const MultiplyAddCase> cases, size_t stride) {
  for (size_t first = 0; first < cases.size(); ++first) {
    alignas(16) u32 a[4];
    alignas(16) u32 b[4];
    alignas(16) u32 c[4];
    alignas(16) u32 out[4];
    const MultiplyAddCase* picked[4];
    for (size_t lane = 0; lane < 4; ++lane) {
      picked[lane] = &cases[(first + lane * stride) % cases.size()];
      a[lane] = picked[lane]->a;
      b[lane] = picked[lane]->b;
      c[lane] = picked[lane]->c;
    }
    const auto load = [](const u32* words) {
      return simde_mm_castsi128_ps(
          simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(words)));
    };
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out),
                         simde_mm_castps_si128(MultiplyAdd(load(a), load(b), load(c))));
    for (size_t lane = 0; lane < 4; ++lane) {
      CAPTURE(lane, picked[lane]->a, picked[lane]->b, picked[lane]->c);
      CHECK(out[lane] == picked[lane]->out);
    }
  }
}

}  // namespace

REX_TEST_CASE("Vmaddfp matches the sampled results", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::Vmaddfp>(kVmaddfpCases, 1);
}

REX_TEST_CASE("Vnmsubfp matches the sampled results", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::Vnmsubfp>(kVnmsubfpCases, 1);
}

REX_TEST_CASE("Vmaddfp keeps each lane apart in a mixed vector", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::Vmaddfp>(kVmaddfpCases, 97);
}

REX_TEST_CASE("Vnmsubfp keeps each lane apart in a mixed vector", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::Vnmsubfp>(kVnmsubfpCases, 97);
}

REX_TEST_CASE("Vmaddfp and Vnmsubfp keep denormals when VSCR NJ is clear", "[ppc][multiply_add]") {
  const auto load = [](const std::array<u32, 4>& words) {
    alignas(16) std::array<u32, 4> host;
    for (size_t i = 0; i < 4; ++i) {
      host[3 - i] = words[i];
    }
    return simde_mm_castsi128_ps(
        simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(host.data())));
  };
  for (const MultiplyAddNJ0Case& expected : kMultiplyAddNJ0Cases) {
    const simde__m128 result =
        expected.negate
            ? rex::ppc::Vnmsubfp(load(expected.a), load(expected.b), load(expected.c), false)
            : rex::ppc::Vmaddfp(load(expected.a), load(expected.b), load(expected.c), false);
    alignas(16) std::array<u32, 4> host;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host.data()),
                         simde_mm_castps_si128(result));
    for (size_t i = 0; i < 4; ++i) {
      CAPTURE(expected.negate, i, expected.a[i], expected.b[i], expected.c[i]);
      CHECK(host[3 - i] == expected.out[i]);
    }
  }
}
