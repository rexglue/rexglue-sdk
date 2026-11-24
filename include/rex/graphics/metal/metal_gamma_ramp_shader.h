/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_GAMMA_RAMP_SHADER_H_
#define XENIA_GPU_METAL_METAL_GAMMA_RAMP_SHADER_H_

// The standard 256-entry gamma ramp shader, shared verbatim by the command
// processor and by conformance.
//
// It lives in a header specifically so the tested source and the shipped source
// cannot drift: a copy in the test would keep passing after production changed.
// Mutating this string therefore fails conformance, which is what makes the
// mutation checks meaningful.
//
// Contract (see metal_gamma_ramp_apply.h for the oracle and the reasoning):
//   idx_c = uint(src_c * 255 + 0.5), out_c = table[idx_c].color_10_c / 1023,
//   alpha 1.0, no interpolation, no saturate.
//
// Channel order: DC_LUT_30_COLOR packs blue at bits 0:9, green at 10:19, red at
// 20:29. Red is read from bits 20:29 -- NOT reversed. D3D12 appears to reverse
// only because its typed R10G10B10A2 view had already permuted the fields.
//
// read() rather than sample() is load-bearing: it guarantees no filtering,
// matching the reference's texel_fetch.

#ifndef __OBJC__
#error "metal_gamma_ramp_shader.h is Objective-C only; include it from .mm"
#endif

#import <Foundation/Foundation.h>

namespace rex {
namespace graphics {
namespace metal {

static NSString* const kMetalGammaRampShader = @R"METAL(
  #include <metal_stdlib>
  using namespace metal;
  struct XeGammaVOut { float4 pos [[position]]; };
  vertex XeGammaVOut xe_gamma_vs(uint vid [[vertex_id]]) {
    XeGammaVOut o;
    float2 t = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
    return o;
  }
  fragment float4 xe_gamma_fs(XeGammaVOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              const device uint* ramp [[buffer(0)]]) {
    float4 c = src.read(uint2(in.pos.xy));
    uint ir = uint(c.r * 255.0 + 0.5);
    uint ig = uint(c.g * 255.0 + 0.5);
    uint ib = uint(c.b * 255.0 + 0.5);
    return float4(float((ramp[ir] >> 20) & 0x3FFu) / 1023.0,
                  float((ramp[ig] >> 10) & 0x3FFu) / 1023.0,
                  float( ramp[ib]        & 0x3FFu) / 1023.0,
                  1.0);
  }
)METAL";

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_GAMMA_RAMP_SHADER_H_
