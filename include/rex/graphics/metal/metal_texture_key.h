/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_KEY_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_KEY_H_

// Legacy Metal texture cache identity
// (openspec/changes/implement-metal-texture-key-mip-range).
//
// This is the smallest production-owned piece of the key: the composition
// itself, with no fetch-constant parsing and no cache state. It exists as a
// header so conformance can exercise the REAL key construction instead of
// re-implementing it — a re-implemented key would agree with itself forever
// while production drifted.
//
// The caller supplies the NORMALIZED EFFECTIVE mip range (from
// texture_util::GetSubresourcesFromFetchConstant), never raw fetch fields.
// Normalization is what lets equivalent ranges share one entry: an absent mip
// page collapses the range to the base level, and both ends are bounded by the
// size-derived maximum. Passing raw register levels here is a defect, and the
// conformance suite mutation-tests exactly that substitution.

#include <cstdint>

#include "xxhash.h"

namespace rex {
namespace graphics {
namespace metal {

// Every field that distinguishes one host texture's storage from another.
//
// The raw mip address (page), the raw packed-mips flag and the stacked flag
// became DIRECT inputs on 2026-10-26: two fetches that differ only there
// describe different guest storage, and the shared D3D12 TextureKey carries
// mip_page and packed_mips the same way. 3D/stack depth reaches the key through
// format_tiled_endian.
struct MetalTextureKeyInputs {
  uint32_t base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch_texels = 0;
  // Packed format/signedness/dimensionality/tiling/endianness, as the cache
  // composes it.
  uint32_t format_tiled_endian = 0;
  // The 12-bit guest fetch swizzle baked into the sampled view.
  uint32_t swizzle = 0;
  // NORMALIZED effective range. Not the raw fetch fields.
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  // Raw fetch fields (mip page, packed_mips, stacked).
  uint32_t mip_address = 0;
  uint32_t packed_mips = 0;
  uint32_t stacked = 0;
};

inline uint64_t MakeMetalTextureKey(const MetalTextureKeyInputs& inputs) {
  struct KeyBytes {
    uint32_t base, width, height, pitch;
    uint32_t format_tiled_endian;
    uint32_t swizzle;
    uint32_t mip_level_range;
    uint32_t mip_address;
    uint32_t packed_stacked;
  } kb{inputs.base,
       inputs.width,
       inputs.height,
       inputs.pitch_texels,
       inputs.format_tiled_endian,
       inputs.swizzle,
       (inputs.mip_min_level << 8) | (inputs.mip_max_level & 0xFF),
       inputs.mip_address,
       (inputs.packed_mips & 1u) | ((inputs.stacked & 1u) << 1)};
  return XXH3_64bits(&kb, sizeof(kb));
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_KEY_H_
