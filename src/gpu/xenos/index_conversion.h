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
#include <rex/xenos/format.h>

#define REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE 16

namespace rex::gpu {

using SIMDVectorU16 = simde__m128i;
using SIMDVectorU32 = simde__m128i;
inline SIMDVectorU16 ReplicateU16(u16 value) {
  return simde_mm_set1_epi16(static_cast<i16>(value));
}
inline SIMDVectorU32 ReplicateU32(u32 value) {
  return simde_mm_set1_epi32(static_cast<i32>(value));
}
inline SIMDVectorU16 LoadAlignedVectorU16(const u16* source) {
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source));
}
inline SIMDVectorU32 LoadAlignedVectorU32(const u32* source) {
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source));
}
inline void StoreUnalignedVectorU16(u16* dest, SIMDVectorU16 source) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dest), source);
}
inline void StoreUnalignedVectorU32(u32* dest, SIMDVectorU32 source) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dest), source);
}
static_assert(sizeof(SIMDVectorU16) == REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE,
              "REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE must reflect the vector size "
              "used");
static_assert(sizeof(SIMDVectorU32) == REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE,
              "REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE must reflect the vector size "
              "used");
inline constexpr u32 kSIMDVectorU16Elements = sizeof(SIMDVectorU16) / sizeof(u16);
inline constexpr u32 kSIMDVectorU32Elements = sizeof(SIMDVectorU32) / sizeof(u32);

/** Reports whether any 16 bit index equals the reset index. */
bool IsResetIndexUsed(const u16* source, u32 count, u16 reset_index_guest_endian);
/**
 * Reports whether the reset index and 0xFFFF occur in 16 bit indices.
 *
 * @param is_reset_index_used_out Receives whether the reset index occurs.
 * @param is_ffff_used_as_vertex_index_out Receives whether 0xFFFF occurs as a vertex index.
 */
void Get16BitResetIndexUsage(const u16* source, u32 count, u16 reset_index_guest_endian,
                             bool& is_reset_index_used_out, bool& is_ffff_used_as_vertex_index_out);
/**
 * Reports whether any 32 bit index equals the reset index after masking.
 *
 * @param low_bits_mask_guest_endian Mask of the low 24 index bits in guest endian.
 * @return True when the reset index occurs.
 */
bool IsResetIndexUsed(const u32* source, u32 count, u32 reset_index_guest_endian,
                      u32 low_bits_mask_guest_endian);
/**
 * Copies 16 bit indices, replacing the reset index with 0xFFFF.
 *
 * @param dest Receives the copied indices with the reset index replaced by 0xFFFF.
 */
void ReplaceResetIndex16To16(u16* dest, const u16* source, u32 count, u16 reset_index_guest_endian);
/**
 * Converts 16 bit indices to 32 bit, replacing the reset index with 0xFFFFFFFF. A real 0xFFFF
 * index becomes 0x0000FFFF. For use when the reset index is not 0xFFFF and 0xFFFF is also valid.
 *
 * @param dest Receives the 32 bit indices.
 */
void ReplaceResetIndex16To32(u32* dest, const u16* source, u32 count, u16 reset_index_guest_endian);
/**
 * Masks 32 bit indices to 24 bits and replaces the reset index with 0xFFFFFFFF.
 *
 * When the host supports 32 bit indices and the vertex shader swaps them, HostSwap is kNone and
 * the reset index and mask are passed swapped to the title endian. When the host supports only 24
 * bit indices, masking and comparison happen first and HostSwap then swaps the result.
 *
 * @tparam HostSwap The endian swap applied for the host.
 * @param dest Receives the masked indices after the HostSwap swap.
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
  if (count >= kSIMDVectorU32Elements) {
    SIMDVectorU32 reset_index_guest_endian_simd = ReplicateU32(reset_index_guest_endian);
    SIMDVectorU32 low_bits_mask_guest_endian_simd = ReplicateU32(low_bits_mask_guest_endian);
    simde__m128i host_swap_shuffle;
    if constexpr (HostSwap != xenos::Endian::kNone) {
      host_swap_shuffle = simde_mm_set_epi32(
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x0F0E0D0C), HostSwap)),
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x0B0A0908), HostSwap)),
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x07060504), HostSwap)),
          static_cast<i32>(xenos::GPUSwap(static_cast<u32>(0x03020100), HostSwap)));
    }
    while (count >= kSIMDVectorU32Elements) {
      count -= kSIMDVectorU32Elements;
      // The comparison gives 0 or all ones, so the result is index | (index == reset_index).
      SIMDVectorU32 source_simd = LoadAlignedVectorU32(source);
      source += kSIMDVectorU32Elements;
      SIMDVectorU32 result_simd;
      source_simd = simde_mm_and_si128(source_simd, low_bits_mask_guest_endian_simd);
      result_simd = simde_mm_or_si128(
          source_simd, simde_mm_cmpeq_epi32(source_simd, reset_index_guest_endian_simd));
      if constexpr (HostSwap != xenos::Endian::kNone) {
        result_simd = simde_mm_shuffle_epi8(result_simd, host_swap_shuffle);
      }
      StoreUnalignedVectorU32(dest, result_simd);
      dest += kSIMDVectorU32Elements;
    }
  }
  while (count--) {
    u32 index = *(source++) & low_bits_mask_guest_endian;
    *(dest++) = index != reset_index_guest_endian ? xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
  }
}

}  // namespace rex::gpu
