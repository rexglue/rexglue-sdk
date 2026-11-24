/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_FORMAT_DECODE_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_FORMAT_DECODE_H_

#include <cstdint>
#include <cstring>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// CPU texel transforms for guest formats the legacy Metal texture cache serves
// through a decoded host format, written as the exact expressions of the
// shared load shaders so the CPU path and the D3D12/Vulkan GPU path agree:
//   texture_load_depth_unorm.cs.xesl: UNorm24To32(word >> 8)
//   texture_load_depth_float.cs.xesl: Float20e4To32(word >> 8)
// The word is the guest 32-bit texel already brought to host byte order.

inline float DecodeDepthUnormTexel(uint32_t word) {
  return xenos::UNorm24To32(word >> 8u);
}

inline float DecodeDepthFloatTexel(uint32_t word) {
  return xenos::Float20e4To32(word >> 8u);
}

// Composes the host-format channel swizzle (the shared tables' RRRR / RGGG /
// RGBA mapping of Xenos components onto host channels) with the guest fetch
// swizzle. Both are 12-bit Xenos swizzles (3 bits per output component:
// 0-3 = X/Y/Z/W, 4 = 0, 5 = 1). A guest component that selects a channel is
// redirected through the host swizzle; constant selections pass through.
inline uint32_t ComposeGuestHostSwizzle(uint32_t guest_swizzle,
                                        uint32_t host_swizzle) {
  uint32_t out = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t g = (guest_swizzle >> (3 * i)) & 0b111;
    if (g < 4) {
      g = (host_swizzle >> (3 * g)) & 0b111;
    }
    out |= g << (3 * i);
  }
  return out;
}

// CTX1 (Xenos two-channel normal-map compression): one 8-byte 4x4 block after
// the fetch-endian swap. Word 0 holds the endpoints as bytes [g0, r0, g1, r1],
// word 1 the 2-bit codes with texel (row r, col c) at bit 8*r + 2*c. Code ->
// weight of endpoint 1: 0->0, 1->3, 2->1, 3->2; endpoint 0 takes the rest of
// the thirds; result = sum / 3 (texture_load_ctx1.cs.xesl,
// XeCTX1TwoBlocksRowToR8G8). out_rg is 16 texels x (r, g), row-major.
inline void DecodeCtx1Block(const uint8_t* block, uint8_t* out_rg) {
  static const uint32_t kHighWeight[4] = {0u, 3u, 1u, 2u};
  uint32_t w0, w1;
  std::memcpy(&w0, block, 4);
  std::memcpy(&w1, block + 4, 4);
  const uint32_t g0 = w0 & 0xFFu, r0 = (w0 >> 8) & 0xFFu;
  const uint32_t g1 = (w0 >> 16) & 0xFFu, r1 = (w0 >> 24) & 0xFFu;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t wh = kHighWeight[(w1 >> (8 * r + 2 * c)) & 3u];
      uint32_t wl = 3u - wh;
      uint8_t* o = out_rg + (r * 4 + c) * 2;
      o[0] = uint8_t((wl * r0 + wh * r1) / 3u);
      o[1] = uint8_t((wl * g0 + wh * g1) / 3u);
    }
  }
}

// DXT3A (the DXT3 alpha block alone): one 8-byte 4x4 block after the
// fetch-endian swap. Word 0 holds rows 0-1, word 1 rows 2-3; a row is 16 bits
// with texel c's 4-bit alpha at bit 4*c. Expanded to 8 bits as a*17
// (texture_load_dxt3a.cs.xesl, XeDXT3FourBlocksRowToA8). out_a is 16 texels,
// row-major.
inline void DecodeDxt3aBlock(const uint8_t* block, uint8_t* out_a) {
  uint32_t w[2];
  std::memcpy(w, block, 8);
  for (uint32_t r = 0; r < 4; ++r) {
    uint32_t row = (w[r >> 1] >> (16 * (r & 1))) & 0xFFFFu;
    for (uint32_t c = 0; c < 4; ++c) {
      out_a[r * 4 + c] = uint8_t(((row >> (4 * c)) & 0xFu) * 17u);
    }
  }
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_FORMAT_DECODE_H_
