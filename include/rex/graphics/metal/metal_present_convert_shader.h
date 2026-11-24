/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PRESENT_CONVERT_SHADER_H_
#define XENIA_GPU_METAL_METAL_PRESENT_CONVERT_SHADER_H_

// The format-mismatch present shaders, shared verbatim by the command processor
// and by conformance so the tested source is the shipped source. A copy would
// keep passing after production changed.
//
// xe_gray_fs is the legacy grayscale debug visualization, retained as the exact
// rollback path. xe_convert_fs is the per-channel conversion; see
// metal_present_convert.h for the oracle and why it was needed.

#ifndef __OBJC__
#error "metal_present_convert_shader.h is Objective-C only; include from .mm"
#endif

#import <Foundation/Foundation.h>

namespace rex {
namespace graphics {
namespace metal {

static NSString* const kMetalPresentConvertShader = @R"METAL(
          #include <metal_stdlib>
          using namespace metal;
          struct VOut { float4 pos [[position]]; };
          vertex VOut xe_gray_vs(uint vid [[vertex_id]]) {
            VOut o;
            float2 t = float2(float((vid << 1) & 2), float(vid & 2));
            o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
            return o;
          }
          fragment float4 xe_gray_fs(VOut in [[stage_in]],
                                     texture2d_array<float> t [[texture(0)]]) {
            // Resolve snapshots are 2D arrays (one slice) so the texture cache
            // can consume them directly. Present the same slice explicitly.
            float v = t.read(uint2(in.pos.xy), 0, 0).r;
            return float4(v, v, v, 1.0);
          }
          fragment float4 xe_convert_fs(VOut in [[stage_in]],
                                        texture2d_array<float> t [[texture(0)]]) {
            // Per-channel conversion, replacing the grayscale replication.
            // Same texture type and slice as xe_gray_fs so no new failure mode
            // is introduced alongside the colour fix.
            //
            // D3D float->UNORM rules: NaN becomes 0, then clamp to [0,1]. The
            // store to an RGBA8Unorm target performs the clamp and the
            // round-to-nearest, so only NaN needs handling here. Alpha is
            // forced opaque, matching both the grayscale path this replaces and
            // the reference's gamma output.
            float3 c = t.read(uint2(in.pos.xy), 0, 0).rgb;
            c = select(c, float3(0.0), isnan(c));
            return float4(saturate(c), 1.0);
          }
        )METAL";

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PRESENT_CONVERT_SHADER_H_
