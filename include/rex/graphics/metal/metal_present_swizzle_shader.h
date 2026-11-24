/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PRESENT_SWIZZLE_SHADER_H_
#define XENIA_GPU_METAL_METAL_PRESENT_SWIZZLE_SHADER_H_

// Swap-texture channel swizzle applied before presentation.
//
// The reference backends present the frontbuffer through texture fetch
// constant 0 (RequestSwapTexture: the guest format, endian and SWIZZLE the
// title's present shader uses). The Metal present shows the resolve snapshot
// directly, which is the resolve destination representation: copy_dest_swap
// is already packed into its channels. Titles undo that swap with a
// Z,Y,X,1 fetch swizzle (0xA0A) on the swap texture; without applying it the
// picture shows red and blue exchanged (Forza Horizon 2, Halo 3, ODST:
// 10:10:10:2 frontbuffers, 2026-09-18).
//
// Guest swizzle encoding: 3 bits per output channel (x at 0:2, y at 3:5,
// z at 6:8, w at 9:11): 0..3 select R/G/B/A, 4 is constant 0, 5 is constant
// 1; 6 and 7 are invalid and clamp to 0 and 1 like GuestToHostSwizzle.
// read() rather than sample(): texel-exact, no filtering.

#ifndef __OBJC__
#error "metal_present_swizzle_shader.h is Objective-C only; include it from .mm"
#endif

#import <Foundation/Foundation.h>

namespace rex {
namespace graphics {
namespace metal {

static NSString* const kMetalPresentSwizzleShader = @R"METAL(
  #include <metal_stdlib>
  using namespace metal;
  struct XeSwzVOut { float4 pos [[position]]; };
  vertex XeSwzVOut xe_swz_vs(uint vid [[vertex_id]]) {
    XeSwzVOut o;
    float2 t = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
    return o;
  }
  inline float xe_swz_pick(float4 c, uint s) {
    if (s > 5u) s -= 2u;
    switch (s) {
      case 0u: return c.r;
      case 1u: return c.g;
      case 2u: return c.b;
      case 3u: return c.a;
      case 4u: return 0.0;
      default: return 1.0;
    }
  }
  inline float4 xe_swz_apply(float4 c, uint swz) {
    return float4(xe_swz_pick(c, swz & 7u), xe_swz_pick(c, (swz >> 3u) & 7u),
                  xe_swz_pick(c, (swz >> 6u) & 7u),
                  xe_swz_pick(c, (swz >> 9u) & 7u));
  }
  // Resolve snapshots are 2D arrays (one slice, so the texture cache can
  // consume them directly); other present sources are plain 2D. The host
  // picks the entry point matching the source's textureType.
  fragment float4 xe_swz_fs_2d(XeSwzVOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]],
                               constant uint& swz [[buffer(0)]]) {
    return xe_swz_apply(src.read(uint2(in.pos.xy)), swz);
  }
  fragment float4 xe_swz_fs_array(XeSwzVOut in [[stage_in]],
                                  texture2d_array<float> src [[texture(0)]],
                                  constant uint& swz [[buffer(0)]]) {
    return xe_swz_apply(src.read(uint2(in.pos.xy), 0, 0), swz);
  }
)METAL";

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PRESENT_SWIZZLE_SHADER_H_
