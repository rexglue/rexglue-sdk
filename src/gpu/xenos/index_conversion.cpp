#include "gpu/xenos/index_conversion.h"

namespace rex::gpu {

bool IsResetIndexUsed(const u16* source, u32 count, u16 reset_index_guest_endian) {
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    if (*(source++) == reset_index_guest_endian) {
      return true;
    }
  }
  if (count >= kSIMDVectorU16Elements) {
    SIMDVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    while (count >= kSIMDVectorU16Elements) {
      count -= kSIMDVectorU16Elements;
      SIMDVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSIMDVectorU16Elements;
      if (simde_mm_movemask_epi8(
              simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd))) {
        return true;
      }
    }
  }
  while (count--) {
    if (*(source++) == reset_index_guest_endian) {
      return true;
    }
  }
  return false;
}

void Get16BitResetIndexUsage(const u16* source, u32 count, u16 reset_index_guest_endian,
                             bool& is_reset_index_used_out,
                             bool& is_ffff_used_as_vertex_index_out) {
  // Optimized for an unused reset index, so there are no early-outs for a simpler loop body.
  is_ffff_used_as_vertex_index_out = false;
  if (reset_index_guest_endian == UINT16_MAX) {
    is_reset_index_used_out = IsResetIndexUsed(source, count, reset_index_guest_endian);
    return;
  }
  is_reset_index_used_out = false;
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u16 index = *(source++);
    if (index == reset_index_guest_endian) {
      is_reset_index_used_out = true;
    }
    if (index == UINT16_MAX) {
      is_ffff_used_as_vertex_index_out = true;
    }
  }
  if (count >= kSIMDVectorU16Elements) {
    SIMDVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    SIMDVectorU16 ffff_simd = ReplicateU16(UINT16_MAX);
    SIMDVectorU16 is_reset_simd = ReplicateU16(0);
    SIMDVectorU16 is_ffff_simd = ReplicateU16(0);
    while (count >= kSIMDVectorU16Elements) {
      count -= kSIMDVectorU16Elements;
      SIMDVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSIMDVectorU16Elements;
      is_reset_simd = simde_mm_or_si128(
          is_reset_simd, simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd));
      is_ffff_simd = simde_mm_or_si128(is_ffff_simd, simde_mm_cmpeq_epi16(source_simd, ffff_simd));
    }
    if (simde_mm_movemask_epi8(is_reset_simd)) {
      is_reset_index_used_out = true;
    }
    if (simde_mm_movemask_epi8(is_ffff_simd)) {
      is_ffff_used_as_vertex_index_out = true;
    }
  }
  while (count--) {
    u16 index = *(source++);
    if (index == reset_index_guest_endian) {
      is_reset_index_used_out = true;
    }
    if (index == UINT16_MAX) {
      is_ffff_used_as_vertex_index_out = true;
    }
  }
}

bool IsResetIndexUsed(const u32* source, u32 count, u32 reset_index_guest_endian,
                      u32 low_bits_mask_guest_endian) {
  // The Xenos only uses the low 24 bits of the index, so mask before comparing.
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    if ((*(source++) & low_bits_mask_guest_endian) == reset_index_guest_endian) {
      return true;
    }
  }
  if (count >= kSIMDVectorU32Elements) {
    SIMDVectorU32 reset_index_guest_endian_simd = ReplicateU32(reset_index_guest_endian);
    while (count >= kSIMDVectorU32Elements) {
      count -= kSIMDVectorU32Elements;
      SIMDVectorU32 source_simd = LoadAlignedVectorU32(source);
      source += kSIMDVectorU32Elements;
      SIMDVectorU32 low_bits_mask_guest_endian_simd = ReplicateU32(low_bits_mask_guest_endian);
      source_simd = simde_mm_and_si128(source_simd, low_bits_mask_guest_endian_simd);
      if (simde_mm_movemask_epi8(
              simde_mm_cmpeq_epi32(source_simd, reset_index_guest_endian_simd))) {
        return true;
      }
    }
  }
  while (count--) {
    if ((*(source++) & low_bits_mask_guest_endian) == reset_index_guest_endian) {
      return true;
    }
  }
  return false;
}

void ReplaceResetIndex16To16(u16* dest, const u16* source, u32 count,
                             u16 reset_index_guest_endian) {
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT16_MAX;
  }
  if (count >= kSIMDVectorU16Elements) {
    SIMDVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    while (count >= kSIMDVectorU16Elements) {
      count -= kSIMDVectorU16Elements;
      // The comparison gives 0 or all ones, so the result is index | (index == reset_index).
      SIMDVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSIMDVectorU16Elements;
      SIMDVectorU16 result_simd;
      result_simd = simde_mm_or_si128(
          source_simd, simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd));
      StoreUnalignedVectorU16(dest, result_simd);
      dest += kSIMDVectorU16Elements;
    }
  }
  while (count--) {
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT16_MAX;
  }
}

void ReplaceResetIndex16To24(u32* dest, const u16* source, u32 count,
                             u16 reset_index_guest_endian) {
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT32_MAX;
  }
  if (count >= kSIMDVectorU16Elements) {
    SIMDVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    while (count >= kSIMDVectorU16Elements) {
      count -= kSIMDVectorU16Elements;
      SIMDVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSIMDVectorU16Elements;
      simde__m128i are_reset = simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd);
      simde__m128i result = simde_mm_or_si128(source_simd, are_reset);
      StoreUnalignedVectorU32(dest, simde_mm_unpacklo_epi16(result, are_reset));
      StoreUnalignedVectorU32(dest + kSIMDVectorU16Elements / 2,
                              simde_mm_unpackhi_epi16(result, are_reset));
      dest += kSIMDVectorU16Elements;
    }
  }
  while (count--) {
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT32_MAX;
  }
}

template void ReplaceResetIndex32To24<xenos::Endian::kNone>(u32* dest, const u32* source, u32 count,
                                                            u32 reset_index_guest_endian,
                                                            u32 low_bits_mask_guest_endian);
template void ReplaceResetIndex32To24<xenos::Endian::k8in16>(u32* dest, const u32* source,
                                                             u32 count,
                                                             u32 reset_index_guest_endian,
                                                             u32 low_bits_mask_guest_endian);
template void ReplaceResetIndex32To24<xenos::Endian::k8in32>(u32* dest, const u32* source,
                                                             u32 count,
                                                             u32 reset_index_guest_endian,
                                                             u32 low_bits_mask_guest_endian);
template void ReplaceResetIndex32To24<xenos::Endian::k16in32>(u32* dest, const u32* source,
                                                              u32 count,
                                                              u32 reset_index_guest_endian,
                                                              u32 low_bits_mask_guest_endian);

}  // namespace rex::gpu
