/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PRESENT_CONVERT_H_
#define XENIA_GPU_METAL_METAL_PRESENT_CONVERT_H_

// CPU oracle for converting a swap source whose pixel format differs from the
// guest output format (RGBA8Unorm).
//
// WHY THIS EXISTS. When the selected swap source's format does not match the
// mailbox, a cross-format blit is invalid, so the backend rendered the source's
// RED channel replicated to all three outputs -- a path its own comment
// describes as debug visualization. The 2026-07-29 census found a REAL title
// reaching it: Halo 3 presents an RGBA16Float source for 100% of its swaps.
//
// The C2-a gate measured what that costs. Halo 3's source is fully finite,
// 100% nonzero, and 100% CHROMATIC (R, G, B differ in every one of 737,280
// pixels; R[0.068,0.141] G[0.000,0.040] B[0.055,0.128]) -- a dark purple image
// being flattened to grey. The output was 100% grayscale and differed from this
// oracle at 100% of pixels. So the path was discarding real colour, not merely
// rendering a black layer differently.
//
// THE CONVERSION, from the D3D float-to-UNORM rules the reference relies on:
//   NaN -> 0; clamp to [0,1]; round to nearest; alpha forced opaque.
//
// Alpha is forced rather than carried through, matching both the grayscale path
// this replaces and the reference's own gamma output, which writes 1.0.
//
// No exact-half tie case needs agreeing with the device: the shader outputs a
// float and the RGBA8Unorm store performs the clamp and rounding in hardware,
// so this oracle models that store rather than duplicating it.

#include <cmath>
#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

// One channel: float -> UNORM8 under D3D rules.
inline uint8_t PresentConvertChannelToUnorm8(float v) {
  if (std::isnan(v)) {
    return 0;
  }
  if (!(v > 0.0f)) {
    return 0;  // also catches -0.0 and -inf
  }
  if (v >= 1.0f) {
    return 255;  // also catches +inf
  }
  return uint8_t(v * 255.0f + 0.5f);
}

struct PresentConvertPixel {
  uint8_t r, g, b, a;
};

inline PresentConvertPixel PresentConvertOracle(float r, float g, float b) {
  return PresentConvertPixel{PresentConvertChannelToUnorm8(r),
                             PresentConvertChannelToUnorm8(g),
                             PresentConvertChannelToUnorm8(b), 255};
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PRESENT_CONVERT_H_
