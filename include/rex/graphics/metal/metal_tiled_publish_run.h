/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TILED_PUBLISH_RUN_H_
#define XENIA_GPU_METAL_METAL_TILED_PUBLISH_RUN_H_

#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

// Number of consecutive texels of one row that are byte-contiguous in the
// guest 2D tiled layout (texture_address::Tiled2D), for a run that starts on
// a multiple of the returned count. This is the granule a row-wise
// publication may memcpy as one block.
//
// Tiled2D places x bits 0-2 in the low address bits scaled by the texel
// size, so 8 texels are contiguous for every size, but the byte span of
// those 8 texels is 8 << bpp_log2. Address bit 3 is y bit 1 (for 8bpp) or
// falls inside the 8-texel span (for 16bpp and wider), and x bits 3-4 go
// to the pipe bits 6-7 (64 bytes away). Hence:
//   8bpp  -> 8 texels  = 8 bytes  (the next 8 bytes are another row)
//   16bpp -> 8 texels  = 16 bytes
//   32bpp -> 4 texels  = 16 bytes
//   64bpp -> 2 texels  = 16 bytes
//   128bpp -> 1 texel  = 16 bytes
// Assuming a flat 16-byte granule for 8bpp published only every other
// 8-texel group (measured: Dead or Alive 5's k_8 depth-of-field mask was
// half stale, halving the scene through a multiply pass).
inline uint32_t TiledPublishRunTexels(uint32_t bytes_per_texel) {
  if (bytes_per_texel <= 1u) {
    return 8u;
  }
  uint32_t run = 16u / bytes_per_texel;
  return run ? run : 1u;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_TILED_PUBLISH_RUN_H_
