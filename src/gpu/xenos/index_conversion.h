/**
 * @file        gpu/xenos/index_conversion.h
 * @brief       Index scans and reset index replacement
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>
#include <cstdint>

// 128 bit SSSE3 (SSE2 integer comparison, pshufb from SSSE3), as AVX2 is above the minimum.
#include <simde/x86/ssse3.h>

#include <rex/types.h>
#include <rex/xenos/xenos.h>

#define REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE 16

namespace rex::gpu {

using SimdVectorU16 = simde__m128i;
using SimdVectorU32 = simde__m128i;
inline SimdVectorU16 ReplicateU16(u16 value) {
  return simde_mm_set1_epi16(static_cast<i16>(value));
}
inline SimdVectorU32 ReplicateU32(u32 value) {
  return simde_mm_set1_epi32(static_cast<i32>(value));
}
inline SimdVectorU16 LoadAlignedVectorU16(const u16* source) {
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source));
}
inline SimdVectorU32 LoadAlignedVectorU32(const u32* source) {
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source));
}
inline void StoreUnalignedVectorU16(u16* dest, SimdVectorU16 source) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dest), source);
}
inline void StoreUnalignedVectorU32(u32* dest, SimdVectorU32 source) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dest), source);
}
static_assert(sizeof(SimdVectorU16) == REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE,
              "REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE must reflect the vector size "
              "actually used");
static_assert(sizeof(SimdVectorU32) == REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE,
              "REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE must reflect the vector size "
              "actually used");
inline constexpr u32 kSimdVectorU16Elements = sizeof(SimdVectorU16) / sizeof(u16);
inline constexpr u32 kSimdVectorU32Elements = sizeof(SimdVectorU32) / sizeof(u32);

/**
 * Reports whether any 16 bit index equals the reset index.
 *
 * @param source The guest indices.
 * @param count The number of indices.
 * @param reset_index_guest_endian The reset index in guest endian.
 * @return True when the reset index occurs.
 */
bool IsResetUsed(const u16* source, u32 count, u16 reset_index_guest_endian);
/**
 * Reports whether the reset index and 0xFFFF occur in 16 bit indices.
 *
 * @param source The guest indices.
 * @param count The number of indices.
 * @param reset_index_guest_endian The reset index in guest endian.
 * @param is_reset_index_used_out Receives whether the reset index occurs.
 * @param is_ffff_used_as_vertex_index_out Receives whether 0xFFFF occurs as a vertex index.
 */
void Get16BitResetIndexUsage(const u16* source, u32 count, u16 reset_index_guest_endian,
                             bool& is_reset_index_used_out, bool& is_ffff_used_as_vertex_index_out);
/**
 * Reports whether any 32 bit index equals the reset index after masking.
 *
 * @param source The guest indices.
 * @param count The number of indices.
 * @param reset_index_guest_endian The reset index in guest endian.
 * @param low_bits_mask_guest_endian The low 24 bits mask in guest endian.
 * @return True when the reset index occurs.
 */
bool IsResetUsed(const u32* source, u32 count, u32 reset_index_guest_endian,
                 u32 low_bits_mask_guest_endian);
/**
 * Copies 16 bit indices, replacing the reset index with 0xFFFF.
 *
 * @param dest The destination indices.
 * @param source The source indices.
 * @param count The number of indices.
 * @param reset_index_guest_endian The reset index in guest endian.
 */
void ReplaceResetIndex16To16(u16* dest, const u16* source, u32 count, u16 reset_index_guest_endian);
/**
 * Converts 16 bit indices to 32 bit, keeping 0xFFFF as a real index and replacing the reset index
 * with 0xFFFFFFFF. For use when the reset index is not 0xFFFF and 0xFFFF is also a valid index.
 * The SIMD path compares to the reset index as uint16 getting 0 or 0xFFFF, replaces the low 16
 * bits of reset indices with 0xFFFF via OR with the comparison result, and expands to 32 bit with
 * 0xFFFF in the upper 16 bits where the comparison passed. That gives 0xFFFFFFFF for primitive
 * reset and 0x0000#### otherwise, including 0x0000FFFF if the original buffer had 0xFFFF but the
 * reset index differs.
 *
 * @param dest The destination indices.
 * @param source The source indices.
 * @param count The number of indices.
 * @param reset_index_guest_endian The reset index in guest endian.
 */
void ReplaceResetIndex16To24(u32* dest, const u16* source, u32 count, u16 reset_index_guest_endian);
/**
 * Masks 32 bit indices to 24 bits and replaces the reset index with 0xFFFFFFFF. The reset index
 * and the low 24 bits mask are taken explicitly because the function is used 2 ways. Passthrough
 * is for when the vertex shader swaps the indices, with 32 bit indices supported on the host.
 * Then HostSwap is kNone, but the reset index and the low bits mask can be swapped to the title
 * endian. Swapping for the host is for when the host supports only 24 bits of an index. Then
 * masking and comparison happen before applying HostSwap, and the data is swapped according to
 * HostSwap from the PowerPC big endianness to the host GPU little endianness, which matches the
 * Xenos.
 *
 * @tparam HostSwap The endian swap applied for the host.
 * @param dest The destination indices.
 * @param source The source indices.
 * @param count The number of indices.
 * @param reset_index_guest_endian The reset index in guest endian.
 * @param low_bits_mask_guest_endian The low 24 bits mask in guest endian.
 */
template <xenos::Endian HostSwap>
void ReplaceResetIndex32To24(u32* dest, const u32* source, u32 count, u32 reset_index_guest_endian,
                             u32 low_bits_mask_guest_endian) {
  // The Xenos only uses the low 24 bits of the index.
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u32 index = *(source++) & low_bits_mask_guest_endian;
    *(dest++) = index != reset_index_guest_endian ? xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
  }
  if (count >= kSimdVectorU32Elements) {
    SimdVectorU32 reset_index_guest_endian_simd = ReplicateU32(reset_index_guest_endian);
    SimdVectorU32 low_bits_mask_guest_endian_simd = ReplicateU32(low_bits_mask_guest_endian);
    simde__m128i host_swap_shuffle;
    if constexpr (HostSwap != xenos::Endian::kNone) {
      host_swap_shuffle = simde_mm_set_epi32(
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x0F0E0D0C), HostSwap)),
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x0B0A0908), HostSwap)),
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x07060504), HostSwap)),
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x03020100), HostSwap)));
    }
    while (count >= kSimdVectorU32Elements) {
      count -= kSimdVectorU32Elements;
      // The comparison gives 0 or all ones, so the result is index | (index == reset_index).
      SimdVectorU32 source_simd = LoadAlignedVectorU32(source);
      source += kSimdVectorU32Elements;
      SimdVectorU32 result_simd;
      source_simd = simde_mm_and_si128(source_simd, low_bits_mask_guest_endian_simd);
      result_simd = simde_mm_or_si128(
          source_simd, simde_mm_cmpeq_epi32(source_simd, reset_index_guest_endian_simd));
      if constexpr (HostSwap != xenos::Endian::kNone) {
        result_simd = simde_mm_shuffle_epi8(result_simd, host_swap_shuffle);
      }
      StoreUnalignedVectorU32(dest, result_simd);
      dest += kSimdVectorU32Elements;
    }
  }
  while (count--) {
    u32 index = *(source++) & low_bits_mask_guest_endian;
    *(dest++) = index != reset_index_guest_endian ? xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
  }
}

}  // namespace rex::gpu
