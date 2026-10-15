/**
 * @file        tests/unit/ppc/dot_product_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <span>

#include "rex_test.h"

#include <rex/ppc/dot_product.h>

#include "dot_product_cases.h"
#include "dot_product_nj0_cases.h"

namespace {

using DotFunction = simde__m128 (*)(simde__m128, simde__m128);

/**
 * Checks the broadcast result of a dot product for each case.
 *
 * @param dot The function under test.
 * @param cases The title element words and the expected result.
 */
void CheckCases(DotFunction dot, std::span<const DotProductCase> cases) {
  for (const DotProductCase& expected : cases) {
    alignas(16) std::array<u32, 4> a;
    alignas(16) std::array<u32, 4> b;
    alignas(16) std::array<u32, 4> out;
    for (size_t i = 0; i < 4; ++i) {
      a[3 - i] = expected.a[i];
      b[3 - i] = expected.b[i];
    }
    simde_mm_store_si128(
        reinterpret_cast<simde__m128i*>(out.data()),
        simde_mm_castps_si128(dot(simde_mm_castsi128_ps(simde_mm_load_si128(
                                      reinterpret_cast<const simde__m128i*>(a.data()))),
                                  simde_mm_castsi128_ps(simde_mm_load_si128(
                                      reinterpret_cast<const simde__m128i*>(b.data()))))));
    CAPTURE(expected.a[0], expected.a[1], expected.a[2], expected.a[3], expected.b[0],
            expected.b[1], expected.b[2], expected.b[3]);
    for (const u32 lane : out) {
      CHECK(lane == expected.out);
    }
  }
}

}  // namespace

REX_TEST_CASE("Vmsum3fp128 matches the sampled results", "[ppc][dot_product]") {
  CheckCases(rex::ppc::Vmsum3fp128, kVmsum3fp128Cases);
}

REX_TEST_CASE("Vmsum4fp128 matches the sampled results", "[ppc][dot_product]") {
  CheckCases(rex::ppc::Vmsum4fp128, kVmsum4fp128Cases);
}

namespace {

/** A deterministic generator whose sequence is fixed by its initial state. */
class SplitMix {
 public:
  explicit SplitMix(u64 seed) : state_(seed) {}

  /**
   * Draws the next word.
   *
   * @return The word.
   */
  u64 operator()() {
    u64 z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

 private:
  u64 state_;
};

/**
 * Draws an operand word from a mix of ordinary values, zeros, denormals, infinities and NaNs.
 *
 * @param rng The random source.
 * @return The operand bits.
 */
u32 RandomOperand(SplitMix& rng) {
  const u32 sign = static_cast<u32>(rng() & 1) << 31;
  const u32 mantissa = static_cast<u32>(rng()) & 0x7FFFFFu;
  switch (rng() % 16) {
    case 0:
      return sign;
    case 1:
      return sign | mantissa;
    case 2:
      return sign | 0x7F800000u;
    case 3:
      return sign | 0x7F800000u | mantissa;
    case 4:
    case 5:
      return sign | ((124u + static_cast<u32>(rng() % 5)) << 23) | mantissa;
    default:
      return sign | ((1u + static_cast<u32>(rng() % 254)) << 23) | mantissa;
  }
}

/**
 * Compares a vector dot product with the reference over random operands.
 *
 * @tparam Terms The number of terms, 3 or 4.
 * @param dot The function under test.
 */
template <u32 Terms>
void CheckAgainstReference(DotFunction dot) {
  SplitMix rng(2024);
  for (u32 iteration = 0; iteration < 200000; ++iteration) {
    alignas(16) std::array<u32, 4> a;
    alignas(16) std::array<u32, 4> b;
    for (u32 i = 0; i < 4; ++i) {
      a[i] = RandomOperand(rng);
      b[i] = RandomOperand(rng);
    }
    std::array<u32, Terms> terms_a;
    std::array<u32, Terms> terms_b;
    for (u32 i = 0; i < Terms; ++i) {
      terms_a[i] = a[3 - i];
      terms_b[i] = b[3 - i];
    }
    alignas(16) std::array<u32, 4> out;
    simde_mm_store_si128(
        reinterpret_cast<simde__m128i*>(out.data()),
        simde_mm_castps_si128(dot(simde_mm_castsi128_ps(simde_mm_load_si128(
                                      reinterpret_cast<const simde__m128i*>(a.data()))),
                                  simde_mm_castsi128_ps(simde_mm_load_si128(
                                      reinterpret_cast<const simde__m128i*>(b.data()))))));
    const u32 expected = rex::ppc::detail::DotProductReference<Terms>(terms_a, terms_b);
    CAPTURE(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
    REQUIRE(out[0] == expected);
    REQUIRE(out[3] == expected);
  }
}

}  // namespace

REX_TEST_CASE("Vmsum3fp128 matches the scalar reference on random operands", "[ppc][dot_product]") {
  CheckAgainstReference<3>(rex::ppc::Vmsum3fp128);
}

REX_TEST_CASE("Vmsum4fp128 matches the scalar reference on random operands", "[ppc][dot_product]") {
  CheckAgainstReference<4>(rex::ppc::Vmsum4fp128);
}

REX_TEST_CASE(
    "Vmsum3fp128 and Vmsum4fp128 take a denormal operand at its value when VSCR NJ is clear",
    "[ppc][dot_product]") {
  const auto load = [](const std::array<u32, 4>& words) {
    alignas(16) std::array<u32, 4> host;
    for (size_t i = 0; i < 4; ++i) {
      host[3 - i] = words[i];
    }
    return simde_mm_castsi128_ps(
        simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(host.data())));
  };
  for (const DotProductNJ0Case& expected : kDotProductNJ0Cases) {
    const simde__m128 result =
        expected.terms == 3 ? rex::ppc::Vmsum3fp128(load(expected.a), load(expected.b), false)
                            : rex::ppc::Vmsum4fp128(load(expected.a), load(expected.b), false);
    alignas(16) std::array<u32, 4> host;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host.data()),
                         simde_mm_castps_si128(result));
    CAPTURE(expected.terms, expected.a[0], expected.a[1], expected.a[2], expected.a[3],
            expected.b[0], expected.b[1], expected.b[2], expected.b[3]);
    CHECK(host[0] == expected.out);
    CHECK(host[3] == expected.out);
  }
}
