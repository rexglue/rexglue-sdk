/**
 * @file        tests/unit/ppc/multiply_add_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstddef>
#include <span>

#include "rex_test.h"

#include <rex/ppc/fpu.h>

#include "multiply_add_cases.h"
#include "multiply_add_nj0_cases.h"

namespace {

using MultiplyAddFunction = simde__m128 (*)(simde__m128, simde__m128, simde__m128);

/** Takes the cases of each vector at the given stride. */
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

/** Returns the next splitmix64 output. */
u64 Next(u64& state) {
  u64 z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/** Builds an f32 from a random draw and a biased exponent of 0 through 255. */
u32 Compose(u64 draw, u32 exponent) {
  return (static_cast<u32>(draw >> 40) & 0x80000000u) | (exponent << 23) |
         (static_cast<u32>(draw) & 0x7FFFFFu);
}

template <bool Negate>
bool SameAsFull(const std::array<u32, 4>& a, const std::array<u32, 4>& b,
                const std::array<u32, 4>& c, bool flush) {
  const auto load = [](const std::array<u32, 4>& words) {
    return simde_mm_castsi128_ps(
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(words.data())));
  };
  std::array<u32, 4> fast;
  std::array<u32, 4> full;
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(fast.data()),
                        simde_mm_castps_si128(rex::ppc::simd::detail::MultiplyAdd<Negate>(
                            load(a), load(b), load(c), flush)));
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(full.data()),
                        simde_mm_castps_si128(rex::ppc::simd::detail::MultiplyAddFull<Negate>(
                            load(a), load(b), load(c), flush)));
  return fast == full;
}

/** Uses random operands where the addend exponent is near the product exponent. */
template <bool Negate>
void CheckAgainstFull(bool flush) {
  u64 state = 0x1234ABCDull + (Negate ? 1 : 0) + (flush ? 2 : 0);
  for (const char* name = REXGLUE_BUILD_CONFIG; *name; ++name) {
    state = state * 131 + static_cast<u64>(*name);
  }
  constexpr u32 kSpecial[] = {0, 1, 2, 126, 127, 253, 254, 255};
  for (size_t i = 0; i < 25000; ++i) {
    std::array<u32, 4> a;
    std::array<u32, 4> b;
    std::array<u32, 4> c;
    for (size_t lane = 0; lane < 4; ++lane) {
      const u64 mode = Next(state) % 16;
      u32 ea = 1 + static_cast<u32>(Next(state) % 254);
      u32 eb = 1 + static_cast<u32>(Next(state) % 254);
      if (mode < 12) {
        ea = 60 + static_cast<u32>(Next(state) % 130);
        eb = 60 + static_cast<u32>(Next(state) % 130);
      }
      i32 ec = static_cast<i32>(ea) + static_cast<i32>(eb) - 127 - 8 +
               static_cast<i32>(Next(state) % 40);
      if (mode >= 12 || ec < 0 || ec > 255) {
        ec = static_cast<i32>(1 + Next(state) % 254);
      }
      a[lane] = Compose(Next(state), ea);
      b[lane] = Compose(Next(state), eb);
      c[lane] = Compose(Next(state), static_cast<u32>(ec));
      if (mode == 15) {
        std::array<u32, 4>* picked[] = {&a, &b, &c};
        (*picked[Next(state) % 3])[lane] = Compose(Next(state), kSpecial[Next(state) % 8]);
      }
    }
    CAPTURE(i, flush, a, b, c);
    REQUIRE(SameAsFull<Negate>(a, b, c, flush));
  }
}

/** Uses operands at or below the smallest normal. */
template <bool Negate>
void CheckTinyAgainstFull() {
  u64 state = 0x7157AB1Eull + (Negate ? 1 : 0);
  for (const char* name = REXGLUE_BUILD_CONFIG; *name; ++name) {
    state = state * 131 + static_cast<u64>(*name);
  }
  const auto mantissa = [&state]() -> u32 {
    switch (Next(state) % 4) {
      case 0:
        return 0;
      case 1:
        return 0x7FFFFF;
      case 2:
        return 1;
      default:
        return static_cast<u32>(Next(state)) & 0x7FFFFF;
    }
  };
  for (size_t i = 0; i < 25000; ++i) {
    std::array<u32, 4> a;
    std::array<u32, 4> b;
    std::array<u32, 4> c;
    for (size_t lane = 0; lane < 4; ++lane) {
      const u32 ea = 1 + static_cast<u32>(Next(state) % 253);
      const i32 wanted = 128 - static_cast<i32>(ea) + static_cast<i32>(Next(state) % 9) - 4;
      const u32 eb = static_cast<u32>(wanted < 0 ? 0 : (wanted > 254 ? 254 : wanted));
      u32 ec = 0;
      switch (Next(state) % 4) {
        case 0:
          ec = 0;
          break;
        case 1:
          ec = static_cast<u32>(Next(state) % 4);
          break;
        case 2:
          ec = ea + eb >= 127 + 25 ? ea + eb - 127 - 24 + static_cast<u32>(Next(state) % 3) : 0;
          break;
        default:
          ec = static_cast<u32>(Next(state) % 8);
          break;
      }
      a[lane] = (static_cast<u32>(Next(state) >> 40) & 0x80000000u) | (ea << 23) | mantissa();
      b[lane] = (static_cast<u32>(Next(state) >> 40) & 0x80000000u) | (eb << 23) | mantissa();
      c[lane] = (static_cast<u32>(Next(state) >> 40) & 0x80000000u) | (ec << 23) | mantissa();
    }
    CAPTURE(i, a, b, c);
    REQUIRE(SameAsFull<Negate>(a, b, c, true));
  }
}

/** The x86 control word bits for flush to zero and denormals are zero. */
constexpr unsigned int kHostFlushBits = 0x8040;

class HostFlushScope {
 public:
  explicit HostFlushScope(bool on) : saved_(simde_mm_getcsr()) {
    simde_mm_setcsr(on ? (saved_ | kHostFlushBits) : (saved_ & ~kHostFlushBits));
  }
  ~HostFlushScope() { simde_mm_setcsr(saved_); }
  HostFlushScope(const HostFlushScope&) = delete;
  HostFlushScope& operator=(const HostFlushScope&) = delete;

 private:
  unsigned int saved_;
};

}  // namespace

REX_TEST_CASE("MultiplyAdd matches the full path with host flush bits off", "[ppc][multiply_add]") {
  HostFlushScope scope(false);
  CheckAgainstFull<false>(true);
  CheckAgainstFull<true>(true);
}

REX_TEST_CASE("MultiplyAdd matches the full path with host flush bits on", "[ppc][multiply_add]") {
  HostFlushScope scope(true);
  CheckAgainstFull<false>(true);
  CheckAgainstFull<true>(true);
}

REX_TEST_CASE("MultiplyAdd matches the full path at the smallest normal with host flush bits off",
              "[ppc][multiply_add]") {
  HostFlushScope scope(false);
  CheckTinyAgainstFull<false>();
  CheckTinyAgainstFull<true>();
}

REX_TEST_CASE("MultiplyAdd matches the full path at the smallest normal with host flush bits on",
              "[ppc][multiply_add]") {
  HostFlushScope scope(true);
  CheckTinyAgainstFull<false>();
  CheckTinyAgainstFull<true>();
}

REX_TEST_CASE("MultiplyAdd matches the sampled results whatever the host flush bits are",
              "[ppc][multiply_add]") {
  for (const bool host_flush : {false, true}) {
    HostFlushScope scope(host_flush);
    CAPTURE(host_flush);
    CheckLanes<rex::ppc::simd::MultiplyAdd>(kMultiplyAddCases, 1);
    CheckLanes<rex::ppc::simd::NegativeMultiplySubtract>(kNegativeMultiplySubtractCases, 1);
  }
}

REX_TEST_CASE("MultiplyAdd matches the full path over exponents near the exact range",
              "[ppc][multiply_add]") {
  CheckAgainstFull<false>(true);
  CheckAgainstFull<false>(false);
}

REX_TEST_CASE("NegativeMultiplySubtract matches the full path over exponents near the exact range",
              "[ppc][multiply_add]") {
  CheckAgainstFull<true>(true);
  CheckAgainstFull<true>(false);
}

REX_TEST_CASE("MultiplyAdd matches the full path at every exponent gap", "[ppc][multiply_add]") {
  u64 state = 99;
  for (i32 gap = -10; gap <= 34; ++gap) {
    for (size_t i = 0; i < 4000; ++i) {
      const u32 ea = 100 + static_cast<u32>(Next(state) % 50);
      const u32 eb = 100 + static_cast<u32>(Next(state) % 50);
      const u32 ec = static_cast<u32>(static_cast<i32>(ea + eb) - 127 - gap);
      const std::array<u32, 4> a{Compose(Next(state), ea), Compose(Next(state), ea),
                                 Compose(Next(state), ea), Compose(Next(state), ea)};
      const std::array<u32, 4> b{Compose(Next(state), eb), Compose(Next(state), eb),
                                 Compose(Next(state), eb), Compose(Next(state), eb)};
      const std::array<u32, 4> c{Compose(Next(state), ec), Compose(Next(state), ec),
                                 Compose(Next(state), ec), Compose(Next(state), ec)};
      CAPTURE(gap, a, b, c);
      REQUIRE(SameAsFull<false>(a, b, c, true));
      REQUIRE(SameAsFull<true>(a, b, c, true));
    }
  }
}

REX_TEST_CASE("MultiplyAdd matches the sampled results", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::simd::MultiplyAdd>(kMultiplyAddCases, 1);
}

REX_TEST_CASE("NegativeMultiplySubtract matches the sampled results", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::simd::NegativeMultiplySubtract>(kNegativeMultiplySubtractCases, 1);
}

REX_TEST_CASE("MultiplyAdd keeps each lane apart in a mixed vector", "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::simd::MultiplyAdd>(kMultiplyAddCases, 97);
}

REX_TEST_CASE("NegativeMultiplySubtract keeps each lane apart in a mixed vector",
              "[ppc][multiply_add]") {
  CheckLanes<rex::ppc::simd::NegativeMultiplySubtract>(kNegativeMultiplySubtractCases, 97);
}

REX_TEST_CASE("MultiplyAdd and NegativeMultiplySubtract keep denormals when VSCR NJ is clear",
              "[ppc][multiply_add]") {
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
        expected.negate ? rex::ppc::simd::NegativeMultiplySubtract(
                              load(expected.a), load(expected.b), load(expected.c), false)
                        : rex::ppc::simd::MultiplyAdd(load(expected.a), load(expected.b),
                                                      load(expected.c), false);
    alignas(16) std::array<u32, 4> host;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host.data()),
                         simde_mm_castps_si128(result));
    for (size_t i = 0; i < 4; ++i) {
      CAPTURE(expected.negate, i, expected.a[i], expected.b[i], expected.c[i]);
      CHECK(host[3 - i] == expected.out[i]);
    }
  }
}
