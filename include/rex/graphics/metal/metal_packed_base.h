/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PACKED_BASE_H_
#define XENIA_GPU_METAL_METAL_PACKED_BASE_H_

#include <algorithm>
#include <cstdint>

#include "rex/graphics/texture_util.h"
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Where the BASE level of a guest texture lives inside its own storage.
//
// Xenos packs every level whose shorter side is <= 16 texels into one
// 32x32-block tail. When the base level itself is that small
// (texture_util::GetPackedMipLevel(width, height) == 0) and the fetch declares
// packed mips, the base is NOT at block (0, 0) of base_address: it sits at the
// packed offset texture_util::GetPackedMipOffset reports for level 0 (for a
// square 8x8 DXT1 that is x = 4 blocks, i.e. 32 bytes into the row), while the
// blocks at (0, 0) hold the smaller levels and padding. Reading the base at
// (0, 0) therefore serves the wrong texels — measured on Sonic Generations as
// an 8x8 all-white sky gradient decoding to a black/white checker, painting
// black bands across the sky (openspec diagnose-sonic-black-sky-columns).
//
// The shared D3D12/Vulkan texture path applies this offset through
// GetGuestTextureLayout; this helper is the Metal legacy-cache equivalent so
// the CPU untile, the watched/trace extent and the cache identity all agree.
struct MetalPackedBaseOffset {
  // True when the base level is stored packed and the offsets apply.
  bool packed = false;
  // Block offsets of the base level inside the base tail.
  uint32_t x_blocks = 0;
  uint32_t y_blocks = 0;
};

inline MetalPackedBaseOffset ComputeMetalPackedBaseOffset(
    bool packed_mips, xenos::DataDimension dimension, uint32_t width,
    uint32_t height, xenos::TextureFormat format) {
  MetalPackedBaseOffset result;
  // A 3D base level packs the same way in X and Y: GetPackedMipOffset only
  // moves along Z for the 1x1 levels deep in the tail, never for level 0, so
  // the base keeps z = 0 and every slice is read at the same (x, y) offset.
  // Halo 4's 16x15x16 luminance LUT is read at x = 16; read at (0, 0, 0) it
  // was mostly the smaller levels' empty space, every lookup returned zero
  // and the game's exposure, and so its whole in-engine image, went black.
  if (!packed_mips || !width || !height) {
    return result;
  }
  if (texture_util::GetPackedMipLevel(width, height) != 0) {
    return result;
  }
  uint32_t x_blocks = 0, y_blocks = 0, z_blocks = 0;
  if (!::rex::xenos::GetPackedMipOffset(width, height, 1, format, 0, x_blocks,
                                        y_blocks, z_blocks)) {
    return result;
  }
  result.packed = true;
  result.x_blocks = x_blocks;
  result.y_blocks = y_blocks;
  return result;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PACKED_BASE_H_
