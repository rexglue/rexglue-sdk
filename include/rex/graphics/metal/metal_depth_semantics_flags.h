/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_DEPTH_SEMANTICS_FLAGS_H_
#define XENIA_GPU_METAL_METAL_DEPTH_SEMANTICS_FLAGS_H_

// Gate parsing and the host-depth ownership predicate for
// implement-metal-depth-semantics, as PURE functions of their inputs so the
// L0 suite pins the full decision matrix without a device or environment.
//
// The two stages interlock: Stage A (XE_METAL_F24_PS) converts depth to
// guest-exact float24 IN the pixel shader, after which host depth carries no
// extra precision worth preserving; Stage B (XE_METAL_HOST_DEPTH) preserves
// host-precision depth across EDRAM aliasing, which is only meaningful when
// Stage A is NOT converting. The predicate therefore fails closed to false
// whenever the float24 PS mode is engaged — a misconfigured arm that sets
// both gates self-disables host-depth ownership rather than double-applying
// depth semantics (canary's exact rule: kD24FS8 && !convert_in_pixel_shader).

#include <cstring>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

enum class MetalF24PsMode { kOff, kCvarDriven, kTruncating, kRounding,
                            kMalformed };

// Pure parse of the XE_METAL_F24_PS value (nullptr = unset). kMalformed is
// distinct so the caller can warn once; behaviorally it means kOff.
inline MetalF24PsMode ParseMetalF24PsMode(const char* value) {
  if (!value || !value[0] || !std::strcmp(value, "0")) {
    return MetalF24PsMode::kOff;
  }
  if (!std::strcmp(value, "1")) return MetalF24PsMode::kCvarDriven;
  if (!std::strcmp(value, "trunc")) return MetalF24PsMode::kTruncating;
  if (!std::strcmp(value, "round")) return MetalF24PsMode::kRounding;
  return MetalF24PsMode::kMalformed;
}

inline bool MetalF24PsEngaged(MetalF24PsMode mode) {
  return mode == MetalF24PsMode::kCvarDriven ||
         mode == MetalF24PsMode::kTruncating ||
         mode == MetalF24PsMode::kRounding;
}

// Combines the XE_METAL_F24_PS env override with the shared
// depth_float24_convert_in_pixel_shader cvar into the single effective mode
// both MetalCommandProcessor and MetalRenderTargetCache must agree on.
//
// - env absent (nullptr): the shared cvars drive it — kCvarDriven when
//   depth_float24_convert_in_pixel_shader is true, kOff otherwise.
// - env present: it is authoritative exactly as ParseMetalF24PsMode says,
//   including an explicit "0" rolling back even a true cvar.
inline MetalF24PsMode EffectiveMetalF24PsMode(
    const char* env_value, bool cvar_depth_float24_convert_in_pixel_shader) {
  if (!env_value) {
    return cvar_depth_float24_convert_in_pixel_shader
               ? MetalF24PsMode::kCvarDriven
               : MetalF24PsMode::kOff;
  }
  return ParseMetalF24PsMode(env_value);
}

// The canary-accurate host-depth ownership predicate (design decision 1):
// true exactly for 20e4 (kD24FS8) when float24 conversion is NOT performed in
// the pixel shader, and only while the Stage B gate is on. Never true for
// kD24S8 — unorm24 round-trips host float32 exactly within its precision, so
// there is no host precision to preserve (edge's unconditional-true is the
// over-trigger this deliberately avoids).
inline bool MetalHostDepthEncodingDifferent(
    bool host_depth_gate_enabled, xenos::DepthRenderTargetFormat format,
    bool f24_ps_engaged) {
  if (!host_depth_gate_enabled || f24_ps_engaged) {
    return false;
  }
  return format == xenos::DepthRenderTargetFormat::kD24FS8;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_DEPTH_SEMANTICS_FLAGS_H_
