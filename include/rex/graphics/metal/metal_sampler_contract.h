/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SAMPLER_CONTRACT_H_
#define XENIA_GPU_METAL_METAL_SAMPLER_CONTRACT_H_

#include <cstdint>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Border colour of a Metal sampler for a guest fetch constant, following the
// shared Direct3D 12 / Vulkan sampler contract
// (D3D12TextureCache::WriteSampler): the fetch's border colour applies only
// when a clamp mode uses the border; k_ABGR_White is (1,1,1,1), k_ABGR_Black
// is (0,0,0,0), the two YCbCr blacks are (0.5,0,0.5,0) / (0,0.5,0.5,0), and
// force_bc_w_to_max sets the border alpha to 1 regardless of the colour.
//
// Metal samplers (and Metal Shader Converter argument-buffer samplers) carry
// only three border colours: transparent black, opaque black and opaque
// white. The ABGR colours and the alpha forcing are exact. The YCbCr blacks
// have no representable chroma (0.5), so they decay to black luma with the
// contract's alpha; `approximated` reports that so the census can count the
// engagement (zero across the 2026-09-04 and 2026-10-26 censuses).
//
// Pure (device-free) so the command processor and the L0 conformance test
// share one definition.
enum class MetalSamplerBorder : uint32_t {
  kTransparentBlack = 0,
  kOpaqueBlack = 1,
  kOpaqueWhite = 2,
};

struct MetalSamplerBorderDecision {
  MetalSamplerBorder border;
  // True when the contract's colour is not representable and the closest
  // Metal border colour (same alpha, black luma) was chosen instead.
  bool approximated;
};

inline MetalSamplerBorderDecision DecideMetalSamplerBorder(
    xenos::BorderColor color, bool force_bc_w_to_max) {
  switch (color) {
    case xenos::BorderColor::k_ABGR_White:
      return {MetalSamplerBorder::kOpaqueWhite, false};
    case xenos::BorderColor::k_ACBYCR_Black:
    case xenos::BorderColor::k_ACBCRY_Black:
      return {force_bc_w_to_max ? MetalSamplerBorder::kOpaqueBlack
                                : MetalSamplerBorder::kTransparentBlack,
              true};
    case xenos::BorderColor::k_ABGR_Black:
    default:
      return {force_bc_w_to_max ? MetalSamplerBorder::kOpaqueBlack
                                : MetalSamplerBorder::kTransparentBlack,
              false};
  }
}

// Sampler key layout bits owned by the border contract (the rest of the key
// is built in MetalCommandProcessor; kept here so the test can pin them).
constexpr uint32_t kMetalSamplerKeyBorderShift = 18;  // 2 bits
constexpr uint32_t kMetalSamplerKeyForceBcWShift = 30;  // 1 bit

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SAMPLER_CONTRACT_H_
