/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_DRAW_RESOLUTION_SCALE_H_
#define XENIA_GPU_METAL_METAL_DRAW_RESOLUTION_SCALE_H_

#include <cstdint>
#include <cstdlib>

namespace rex {
namespace graphics {
namespace metal {

// Draw resolution scaling on the native Metal backend
// (implement-metal-draw-resolution-scaling). The scale is an integer per axis,
// read once per session from draw_resolution_scale_x/y and clamped to
// 1..kMetalMaxDrawResolutionScale (memory: the canonical EDRAM, render targets
// and resolve snapshots all grow by x * y).
constexpr uint32_t kMetalMaxDrawResolutionScale = 3;

struct MetalDrawResolutionScale {
  // The configured scale after clamping.
  uint32_t requested_x = 1;
  uint32_t requested_y = 1;
  // Whether either configured value was outside
  // 1..kMetalMaxDrawResolutionScale.
  bool clamped = false;
  // The scale the session renders at: the requested scale, or 1x1 when
  // scaled rendering is unavailable (the XE_METAL_DRAW_RESOLUTION_SCALE=0 kill
  // switch, or a canonical EDRAM raster rollback).
  uint32_t x = 1;
  uint32_t y = 1;
};

constexpr uint32_t ClampMetalDrawResolutionScaleAxis(int32_t value) {
  return value < 1 ? 1u
                   : value > int32_t(kMetalMaxDrawResolutionScale)
                         ? kMetalMaxDrawResolutionScale
                         : uint32_t(value);
}

constexpr MetalDrawResolutionScale ResolveMetalDrawResolutionScale(
    int32_t config_x, int32_t config_y, bool scaled_rendering_enabled) {
  MetalDrawResolutionScale scale;
  scale.requested_x = ClampMetalDrawResolutionScaleAxis(config_x);
  scale.requested_y = ClampMetalDrawResolutionScaleAxis(config_y);
  scale.clamped = int32_t(scale.requested_x) != config_x ||
                  int32_t(scale.requested_y) != config_y;
  if (scaled_rendering_enabled) {
    scale.x = scale.requested_x;
    scale.y = scale.requested_y;
  }
  return scale;
}

// Exact representative pixel (default on; XE_METAL_SCALE_EXACT_PIXEL=0 is the
// rollback). At an even scale no host pixel center coincides with the guest
// pixel's sample point: the representative (scale >> 1) host pixel samples a
// quarter guest pixel past it on each axis, so resolves wrote guest memory
// from shifted samples (Red Dead Redemption's offscreen chains drifted a
// little more with every pass). With this on, geometry is shifted half a host
// pixel on even-scale axes and the translator's pixel position and
// scaled-texture coordinates match, so the representative host pixel renders
// what 1x renders for its guest pixel. No effect at 1x or on odd scales'
// geometry.
inline bool MetalExactScaledPixelPositionEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("XE_METAL_SCALE_EXACT_PIXEL");
    return !(v && v[0] == '0');
  }();
  return enabled;
}

// The geometry shift in host pixels for an axis at this scale (right and down).
inline float MetalScaledPixelShiftHostPixels(uint32_t scale) {
  return (MetalExactScaledPixelPositionEnabled() && !(scale & 1)) ? 0.5f
                                                                   : 0.0f;
}

// Anisotropic filtering in a scaled session (default off;
// XE_METAL_SCALE_ANISO=1 keeps the guest's anisotropy). Derivatives at scale
// come from host pixels a fraction of a guest pixel apart, so a shader whose
// coordinates jump between guest pixels - Gears of War 3 builds its color
// grading LUT from frac() of an interpolant that wraps just above each slice's
// first row - gets a huge gradient in the representative pixel's quad, and the
// anisotropic footprint smears neighbouring slices into the 1x bytes (the grey
// menu). At 1x the same jump falls between quads. Bilinear sampling is exact
// there, and the supersampling of a scaled session covers most of what
// anisotropy adds (corpus at 2x2: median 1x distance 1.28 with it, 0.55
// without; only Gears 3 changes visibly).
inline bool MetalScaledAnisotropyEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("XE_METAL_SCALE_ANISO");
    return v && v[0] == '1';
  }();
  return enabled;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_DRAW_RESOLUTION_SCALE_H_
