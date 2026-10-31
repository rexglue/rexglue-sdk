/**
 * @file        rex/ppc/dot_product.h
 * @brief       Hardware-matching overflow and one canonical NaN for vmsum3fp128 / vmsum4fp128
 *
 * @copyright   Copyright (c) 2026 Dwreck
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The code generator emits simde_mm_dp_ps(a, b, 0xEF) for vmsum3fp128 and
 *              simde_mm_dp_ps(a, b, 0xFF) for vmsum4fp128 (src/codegen/builders/vector.cpp).
 *              This header redirects that call so its result follows two rules:
 *
 *              1. Overflow of a dot product of finite inputs gives QNaN 0x7FC00000 in every
 *                 result lane, as on hardware (DPPS gives +-inf). The rule is Xenia's
 *                 DOT_PRODUCT_3 / DOT_PRODUCT_4 (src/xenia/cpu/backend/x64/x64_sequences.cc,
 *                 EmitDotProductResult): when the float32 result is not finite, the products
 *                 are summed in float64; if every used input lane is finite, the result is that
 *                 sum converted to float32, or QNaN when the conversion overflows.
 *              2. Every NaN in the result is 0x7FC00000. Hardware most likely propagates an input
 *                 NaN's payload; this deliberately does not. x86 DPPS returns one of the input
 *                 payloads and AMD and Intel pick different ones, and ARM NEON propagates its own
 *                 choice, so a canonical NaN is the only way the bits match on every host CPU.
 *                 An infinity that comes from the inputs passes through unchanged.
 *
 *              Finite results take the DPPS value unchanged. The header is included by
 *              <rex/ppc/intrinsics.h> after simde's headers, so generated code picks it up
 *              through <rex/ppc.h> without any change to the generated text.
 */

#pragma once

#include <cstdint>
#include <cstring>

#include <simde/x86/sse.h>
#include <simde/x86/sse2.h>
#include <simde/x86/sse4.1.h>

// The dot product itself, without the redirect below. The two branches mirror simde's own
// definition in simde/x86/sse4.1.h: with native SSE4.1 simde_mm_dp_ps is a macro for _mm_dp_ps,
// otherwise (ARM NEON, or x86 without SSE4.1) it is an inline function. Neither branch names the
// macro defined at the end of this file, so the redirect cannot recurse.
#if defined(SIMDE_X86_SSE4_1_NATIVE)
#define REX_PPC_RAW_DOT_PRODUCT_PS(a, b, imm8) _mm_dp_ps((a), (b), (imm8))
#else
// The parentheses around the name keep a function-like macro of that name from expanding here.
#define REX_PPC_RAW_DOT_PRODUCT_PS(a, b, imm8) (simde_mm_dp_ps)((a), (b), (imm8))
#endif

namespace rex::ppc {

inline constexpr uint32_t kDotProductQNaN = 0x7FC00000u;

// Replaces every NaN lane of `r` with 0x7FC00000. An unordered compare of the vector with itself
// is all ones exactly in the NaN lanes (quiet or signalling, any payload or sign); the blend takes
// the canonical NaN there and `r` everywhere else, so infinities are kept.
inline simde__m128 CanonicalizeDotProductNaN(simde__m128 r) {
  const simde__m128 is_nan = simde_mm_cmpunord_ps(r, r);
  const simde__m128 canonical =
      simde_mm_castsi128_ps(simde_mm_set1_epi32(static_cast<int>(kDotProductQNaN)));
  return simde_mm_blendv_ps(r, canonical, is_nan);
}

namespace detail {

inline bool IsFiniteFloatBits(uint32_t bits) {
  return (bits & 0x7F800000u) != 0x7F800000u;
}

// The rare path of DotProductPs: `r`, the DPPS result for mask `imm8`, has an infinite or NaN
// lane. Recomputes the result by Xenia's rule (see the file comment).
HEDLEY_NEVER_INLINE inline simde__m128 DotProductNonFiniteResult(simde__m128 a, simde__m128 b,
                                                                 simde__m128 r, int imm8) {
  uint32_t a_bits[4];
  uint32_t b_bits[4];
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(a_bits), simde_mm_castps_si128(a));
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(b_bits), simde_mm_castps_si128(b));
  double products[4] = {};
  for (int i = 0; i < 4; ++i) {
    if (!((imm8 >> (4 + i)) & 1)) {
      continue;
    }
    if (!IsFiniteFloatBits(a_bits[i]) || !IsFiniteFloatBits(b_bits[i])) {
      // An infinity or NaN from the inputs passes through, with NaNs made canonical.
      return CanonicalizeDotProductNaN(r);
    }
    float a_value;
    float b_value;
    std::memcpy(&a_value, &a_bits[i], sizeof(a_value));
    std::memcpy(&b_value, &b_bits[i], sizeof(b_value));
    // Exact: a product of two float32 values fits in a float64.
    products[i] = double(a_value) * double(b_value);
  }
  // Xenia's summation order; its lanes are numbered in the reverse of the host order used here.
  const float sum = float((products[1] + products[3]) + (products[0] + products[2]));
  uint32_t sum_bits;
  std::memcpy(&sum_bits, &sum, sizeof(sum_bits));
  if (!IsFiniteFloatBits(sum_bits)) {
    sum_bits = kDotProductQNaN;
  }
  uint32_t result_bits[4];
  for (int i = 0; i < 4; ++i) {
    result_bits[i] = ((imm8 >> i) & 1) ? sum_bits : 0;
  }
  return simde_mm_castsi128_ps(
      simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(result_bits)));
}

}  // namespace detail

// simde_mm_dp_ps(a, b, kImm8) with the two rules from the file comment. A finite result costs an
// exponent test on top of the dot product.
template <int kImm8>
inline simde__m128 DotProductPs(simde__m128 a, simde__m128 b) {
  const simde__m128 r = REX_PPC_RAW_DOT_PRODUCT_PS(a, b, kImm8);
  const simde__m128i exponent_mask = simde_mm_set1_epi32(0x7F800000);
  const simde__m128i exponent = simde_mm_and_si128(simde_mm_castps_si128(r), exponent_mask);
  if (simde_mm_movemask_epi8(simde_mm_cmpeq_epi32(exponent, exponent_mask)) == 0) {
    return r;
  }
  return detail::DotProductNonFiniteResult(a, b, r, kImm8);
}

}  // namespace rex::ppc

// Every simde_mm_dp_ps in generated code now goes through DotProductPs. A function-like macro, so
// the generated text is unchanged; the mask becomes a template argument, so it stays a
// compile-time constant, and each operand is evaluated once. simde's own macro (native path) is
// dropped first.
#undef simde_mm_dp_ps
#define simde_mm_dp_ps(a, b, imm8) (::rex::ppc::DotProductPs<(imm8)>((a), (b)))
