/**
 * @file        tests/unit/ppc/estimate_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <bit>
#include <cstddef>
#include <span>
#include <vector>

#include "rex_test.h"

#include <rex/ppc/estimate.h>

#include "estimate_cases.h"
#include "estimate_nj0_cases.h"

namespace {

/**
 * Checks the lane results of a vector estimate. Special lanes expect the result of the IEEE mode.
 *
 * @tparam Estimate The vector estimate.
 * @param cases The operand and expected result of each lane.
 */
template <simde__m128 (*Estimate)(simde__m128)>
void CheckVector(std::span<const EstimateCase32> cases) {
  for (size_t first = 0; first < cases.size(); first += 4) {
    alignas(16) u32 in[4];
    alignas(16) u32 out[4];
    for (size_t lane = 0; lane < 4; ++lane) {
      in[lane] = cases[(first + lane) % cases.size()].in;
    }
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out),
                         simde_mm_castps_si128(Estimate(simde_mm_castsi128_ps(
                             simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(in))))));
    for (size_t lane = 0; lane < 4; ++lane) {
      const EstimateCase32& expected = cases[(first + lane) % cases.size()];
      CAPTURE(expected.in);
      CHECK(out[lane] == expected.out);
    }
  }
}

/**
 * Checks the lane results of a vector estimate when the 4 lanes of each vector come from unrelated
 * cases, so that every special and normal combination shares a vector.
 *
 * @tparam Estimate The vector estimate.
 * @param cases The operand and expected result of each lane.
 */
template <simde__m128 (*Estimate)(simde__m128)>
void CheckMixed(std::span<const EstimateCase32> cases) {
  for (size_t first = 0; first < cases.size(); first += 4) {
    alignas(16) u32 in[4];
    alignas(16) u32 out[4];
    const EstimateCase32* picked[4];
    for (size_t lane = 0; lane < 4; ++lane) {
      picked[lane] = &cases[((first + lane) * 2654435761ull) % cases.size()];
      in[lane] = picked[lane]->in;
    }
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out),
                         simde_mm_castps_si128(Estimate(simde_mm_castsi128_ps(
                             simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(in))))));
    for (size_t lane = 0; lane < 4; ++lane) {
      CAPTURE(picked[lane]->in);
      CHECK(out[lane] == picked[lane]->out);
    }
  }
}

/**
 * Reports whether an operand takes the fast path of every vector estimate.
 *
 * @param in The operand bits.
 * @return True when the operand is positive with a magnitude from 2^-126 up to the vexptefp bound.
 */
bool IsRegular(u32 in) {
  return in >= 0x00800000u && in <= 0x42FBFFFFu;
}

/**
 * Runs a vector estimate on 4 cases and checks each lane.
 *
 * @tparam Estimate The vector estimate.
 * @param picked The case of each lane.
 */
template <simde__m128 (*Estimate)(simde__m128)>
void CheckLanes(const EstimateCase32* const (&picked)[4]) {
  alignas(16) u32 in[4];
  alignas(16) u32 out[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    in[lane] = picked[lane]->in;
  }
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out),
                       simde_mm_castps_si128(Estimate(simde_mm_castsi128_ps(
                           simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(in))))));
  for (size_t lane = 0; lane < 4; ++lane) {
    CAPTURE(lane, picked[lane]->in);
    CHECK(out[lane] == picked[lane]->out);
  }
}

/**
 * Checks a vector estimate on vectors whose 4 lanes all take the fast path.
 *
 * @tparam Estimate The vector estimate.
 * @param cases The operand and expected result of each lane.
 */
template <simde__m128 (*Estimate)(simde__m128)>
void CheckRegular(std::span<const EstimateCase32> cases) {
  std::vector<const EstimateCase32*> regular;
  for (const EstimateCase32& c : cases) {
    if (IsRegular(c.in)) {
      regular.push_back(&c);
    }
  }
  REQUIRE(regular.size() >= 4);
  for (size_t first = 0; first < regular.size(); first += 4) {
    const EstimateCase32* const picked[4] = {regular[first], regular[(first + 1) % regular.size()],
                                             regular[(first + 2) % regular.size()],
                                             regular[(first + 3) % regular.size()]};
    CheckLanes<Estimate>(picked);
  }
}

/**
 * Checks each case in each lane position of a vector whose other lanes take the fast path.
 *
 * @tparam Estimate The vector estimate.
 * @param cases The operand and expected result of each lane.
 */
template <simde__m128 (*Estimate)(simde__m128)>
void CheckEachLane(std::span<const EstimateCase32> cases) {
  const auto filler =
      std::ranges::find_if(cases, [](const EstimateCase32& c) { return IsRegular(c.in); });
  REQUIRE(filler != cases.end());
  for (const EstimateCase32& c : cases) {
    for (size_t position = 0; position < 4; ++position) {
      const EstimateCase32* picked[4] = {&*filler, &*filler, &*filler, &*filler};
      picked[position] = &c;
      CheckLanes<Estimate>(picked);
    }
  }
}

/**
 * Checks the results of a scalar estimate.
 *
 * @tparam Estimate The scalar estimate.
 * @param cases The operand and expected result bits.
 */
template <f64 (*Estimate)(f64)>
void CheckScalar(std::span<const EstimateCase64> cases) {
  for (const EstimateCase64& expected : cases) {
    CAPTURE(expected.in);
    CHECK(std::bit_cast<u64>(Estimate(std::bit_cast<f64>(expected.in))) == expected.out);
  }
}

}  // namespace

REX_TEST_CASE("scalar ReciprocalEstimate matches the sampled results", "[ppc][estimate]") {
  CheckScalar<rex::ppc::ReciprocalEstimate>(kScalarReciprocalEstimateCases);
}

REX_TEST_CASE("scalar ReciprocalSqrtEstimate matches the sampled results", "[ppc][estimate]") {
  CheckScalar<rex::ppc::ReciprocalSqrtEstimate>(kScalarReciprocalSqrtEstimateCases);
}

REX_TEST_CASE("simd ReciprocalEstimate matches the sampled results", "[ppc][estimate]") {
  CheckVector<rex::ppc::simd::ReciprocalEstimate>(kReciprocalEstimateCases);
}

REX_TEST_CASE("simd ReciprocalSqrtEstimate matches the sampled results", "[ppc][estimate]") {
  CheckVector<rex::ppc::simd::ReciprocalSqrtEstimate>(kReciprocalSqrtEstimateCases);
}

REX_TEST_CASE("simd Log2Estimate matches the sampled results", "[ppc][estimate]") {
  CheckVector<rex::ppc::simd::Log2Estimate>(kLog2EstimateCases);
}

REX_TEST_CASE("simd Exp2Estimate matches the sampled results", "[ppc][estimate]") {
  CheckVector<rex::ppc::simd::Exp2Estimate>(kExp2EstimateCases);
}

REX_TEST_CASE("simd ReciprocalEstimate keeps each lane apart in a mixed vector",
              "[ppc][estimate]") {
  CheckMixed<rex::ppc::simd::ReciprocalEstimate>(kReciprocalEstimateCases);
}

REX_TEST_CASE("simd ReciprocalSqrtEstimate keeps each lane apart in a mixed vector",
              "[ppc][estimate]") {
  CheckMixed<rex::ppc::simd::ReciprocalSqrtEstimate>(kReciprocalSqrtEstimateCases);
}

REX_TEST_CASE("simd Log2Estimate keeps each lane apart in a mixed vector", "[ppc][estimate]") {
  CheckMixed<rex::ppc::simd::Log2Estimate>(kLog2EstimateCases);
}

REX_TEST_CASE("simd Exp2Estimate keeps each lane apart in a mixed vector", "[ppc][estimate]") {
  CheckMixed<rex::ppc::simd::Exp2Estimate>(kExp2EstimateCases);
}

REX_TEST_CASE("simd ReciprocalEstimate matches the sampled results when every lane is regular",
              "[ppc][estimate]") {
  CheckRegular<rex::ppc::simd::ReciprocalEstimate>(kReciprocalEstimateCases);
}

REX_TEST_CASE("simd ReciprocalEstimate matches each sampled result in each lane position",
              "[ppc][estimate]") {
  CheckEachLane<rex::ppc::simd::ReciprocalEstimate>(kReciprocalEstimateCases);
}

REX_TEST_CASE("simd ReciprocalSqrtEstimate matches the sampled results when every lane is regular",
              "[ppc][estimate]") {
  CheckRegular<rex::ppc::simd::ReciprocalSqrtEstimate>(kReciprocalSqrtEstimateCases);
}

REX_TEST_CASE("simd ReciprocalSqrtEstimate matches each sampled result in each lane position",
              "[ppc][estimate]") {
  CheckEachLane<rex::ppc::simd::ReciprocalSqrtEstimate>(kReciprocalSqrtEstimateCases);
}

REX_TEST_CASE("simd Log2Estimate matches the sampled results when every lane is regular",
              "[ppc][estimate]") {
  CheckRegular<rex::ppc::simd::Log2Estimate>(kLog2EstimateCases);
}

REX_TEST_CASE("simd Log2Estimate matches each sampled result in each lane position",
              "[ppc][estimate]") {
  CheckEachLane<rex::ppc::simd::Log2Estimate>(kLog2EstimateCases);
}

REX_TEST_CASE("simd Exp2Estimate matches the sampled results when every lane is regular",
              "[ppc][estimate]") {
  CheckRegular<rex::ppc::simd::Exp2Estimate>(kExp2EstimateCases);
}

REX_TEST_CASE("simd Exp2Estimate matches each sampled result in each lane position",
              "[ppc][estimate]") {
  CheckEachLane<rex::ppc::simd::Exp2Estimate>(kExp2EstimateCases);
}

REX_TEST_CASE("The vector estimates take a denormal operand at its value when VSCR NJ is clear",
              "[ppc][estimate]") {
  for (const EstimateNJ0Case& expected : kEstimateNJ0Cases) {
    alignas(16) std::array<u32, 4> host;
    for (size_t i = 0; i < 4; ++i) {
      host[3 - i] = expected.in[i];
    }
    const auto in = simde_mm_castsi128_ps(
        simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(host.data())));
    simde__m128 result;
    switch (expected.family) {
      case 0:
        result = rex::ppc::simd::ReciprocalEstimate(in, false);
        break;
      case 1:
        result = rex::ppc::simd::ReciprocalSqrtEstimate(in, false);
        break;
      case 2:
        result = rex::ppc::simd::Log2Estimate(in, false);
        break;
      default:
        result = rex::ppc::simd::Exp2Estimate(in, false);
        break;
    }
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host.data()),
                         simde_mm_castps_si128(result));
    for (size_t i = 0; i < 4; ++i) {
      CAPTURE(expected.family, i, expected.in[i]);
      CHECK(host[3 - i] == expected.out[i]);
    }
  }
}
