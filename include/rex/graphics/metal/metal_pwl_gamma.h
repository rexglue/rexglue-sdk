/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PWL_GAMMA_H_
#define XENIA_GPU_METAL_METAL_PWL_GAMMA_H_

// The piecewise-linear (PWL) guest gamma ramp: the CPU oracle and the Metal
// shader, in one header so the tested source and the shipped source cannot
// drift. Conformance compares the device to the oracle pixel for pixel.
//
// THE CONTRACT -- apply_gamma_pwl.xesli (the reference D3D12/Vulkan shader),
// with the increment taken from DC_LUTA_CONTROL:
//
//   in10   = uint(saturate(src_c) * 1023 + 0.5)        // D3D10+ UNORM rules
//   entry  = pwl[(in10 >> 3) * 3 + channel]             // 128 entries x R,G,B
//   out_c  = saturate((entry.base + (in10 & 7) * entry.delta / 8)
//                     / (64 * 1023))
//   out_a  = 1
//
// Channel order is the guest write order, R, G, B (gamma_ramp_pwl_rgb_[128][3]),
// exactly the reference shader's `* 3 + 0/1/2`.
//
// SELECTION (docs/dev/metal_pwl_gamma_audit.md, reopened 2026-10-10): the PWL
// path is used only when DC_LUTA_CONTROL holds 3 (an increment of 2^3 = 8, so
// a 10-bit input), the only PWL value any title has been observed to write.
// Any other nonzero value is logged once and presented through the
// 256-entry table rather than guessed.
//
// PRECISION: the input is read from the swap source at full precision (the
// RGBA16Float resolved snapshot for 10-bit front buffers), not from an 8-bit
// copy, so the 10-bit index is exact; the result is stored to the RGBA8Unorm
// mailbox, which the oracle models. The library is compiled with fast math
// off so the device evaluates the same float expression as the oracle.

#include <cmath>
#include <cstdint>

#include <rex/xenos/register.h>

namespace rex {
namespace graphics {
namespace metal {

inline constexpr uint32_t kPwlGammaEntries = 128;
// One raw DC_LUT_PWL_DATA dword per entry and channel, the guest layout.
inline constexpr uint32_t kPwlGammaTableBytes =
    kPwlGammaEntries * 3 * sizeof(uint32_t);
// The only DC_LUTA_CONTROL value that selects PWL here.
inline constexpr uint32_t kPwlGammaControlIncrement3 = 3;

inline uint32_t PwlGammaInput10(float c) {
  if (!(c > 0.0f)) {
    c = 0.0f;  // NaN and negatives
  }
  if (c > 1.0f) {
    c = 1.0f;
  }
  return uint32_t(c * 1023.0f + 0.5f);
}

// Channel output in [0, 1]; the same expression, in the same order, as the
// shader below.
inline float PwlGammaChannel(const reg::DC_LUT_PWL_DATA* table, float c,
                             uint32_t channel) {
  const uint32_t in10 = PwlGammaInput10(c);
  const reg::DC_LUT_PWL_DATA e = table[(in10 >> 3) * 3 + channel];
  const float base = float(e.base);
  const float delta = float(e.delta);
  float v = (base + float(in10 & 7u) * delta * 0.125f) *
            (1.0f / (64.0f * 1023.0f));
  if (!(v > 0.0f)) {
    v = 0.0f;
  }
  return v > 1.0f ? 1.0f : v;
}

// Float -> UNORM8 as the RGBA8Unorm render-target store performs it: the
// product with 255 is taken exactly (a float multiply can round 42.5000014 to
// 42.5 and flip the result; measured on the M4). No exact tie exists, since
// v * 255 = n + 1/2 needs v = (2n+1)/510, which no binary fraction equals.
inline uint8_t PwlGammaQuantize8(float v) {
  return uint8_t(std::nearbyint(double(v) * 255.0));
}

struct PwlGammaPixel {
  uint8_t r, g, b, a;
};

inline PwlGammaPixel ApplyPwlGammaOracle(const reg::DC_LUT_PWL_DATA* table,
                                         float r, float g, float b) {
  return {PwlGammaQuantize8(PwlGammaChannel(table, r, 0)),
          PwlGammaQuantize8(PwlGammaChannel(table, g, 1)),
          PwlGammaQuantize8(PwlGammaChannel(table, b, 2)), 255};
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace rex {
namespace graphics {
namespace metal {

static NSString* const kMetalPwlGammaShader = @R"METAL(
  #include <metal_stdlib>
  using namespace metal;
  struct XePwlVOut { float4 pos [[position]]; };
  vertex XePwlVOut xe_pwl_vs(uint vid [[vertex_id]]) {
    XePwlVOut o;
    float2 t = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
    return o;
  }
  static float xe_pwl_channel(float c, const device uint* pwl, uint channel) {
    c = isnan(c) ? 0.0 : clamp(c, 0.0, 1.0);
    uint in10 = uint(c * 1023.0 + 0.5);
    uint e = pwl[(in10 >> 3) * 3 + channel];
    float base = float(e & 0xFFFFu);
    float delta = float(e >> 16);
    float v = (base + float(in10 & 7u) * delta * 0.125) *
              (1.0 / (64.0 * 1023.0));
    return clamp(v, 0.0, 1.0);
  }
  static float4 xe_pwl(float3 c, const device uint* pwl) {
    return float4(xe_pwl_channel(c.r, pwl, 0), xe_pwl_channel(c.g, pwl, 1),
                  xe_pwl_channel(c.b, pwl, 2), 1.0);
  }
  // Resolved snapshots are one-slice 2D arrays.
  fragment float4 xe_pwl_fs_array(XePwlVOut in [[stage_in]],
                                  texture2d_array<float> t [[texture(0)]],
                                  const device uint* pwl [[buffer(0)]]) {
    return xe_pwl(t.read(uint2(in.pos.xy), 0, 0).rgb, pwl);
  }
  fragment float4 xe_pwl_fs_2d(XePwlVOut in [[stage_in]],
                               texture2d<float> t [[texture(0)]],
                               const device uint* pwl [[buffer(0)]]) {
    return xe_pwl(t.read(uint2(in.pos.xy)).rgb, pwl);
  }
)METAL";

// Fast math off: the device must evaluate the oracle's expression exactly.
inline MTLCompileOptions* MetalPwlGammaCompileOptions() {
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
  if (@available(macOS 15.0, *)) {
    options.mathMode = MTLMathModeSafe;
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
  }
  return options;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
#endif  // __OBJC__

#endif  // XENIA_GPU_METAL_METAL_PWL_GAMMA_H_
