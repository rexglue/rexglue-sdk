/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PASS_ATTACHMENT_USE_H_
#define XENIA_GPU_METAL_METAL_PASS_ATTACHMENT_USE_H_

#include <cstdint>

#include "rex/graphics/metal/metal_fixed_function_state.h"
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Device-free attachment-use predicate (reduce-metal-draw-pass-traffic D2).
//
// On a tile-based GPU every attachment a render pass binds is loaded into
// tile memory when the pass starts and stored back when it ends, whether or
// not any draw touched it. This header decides, from one draw's normalized
// register state, which attachments that draw CAN read or write; the pass
// binds the union of its draws' use and leaves the rest untouched (their
// textures keep their contents without a load or a store).
//
// The inputs are the NORMALIZED depth control and color mask
// (draw_util::GetNormalizedDepthControl / GetNormalizedColorMask), so a color
// target the pixel shader never writes, or a depth test the EDRAM mode
// disables, already reads as unused here.
//
// Bit layout of an attachment set: depth, stencil, then color 0..3. Depth and
// stencil are separate bits because Metal binds the two planes of a
// Depth32Float_Stencil8 texture independently and Apple GPUs store them
// separately, so a depth-only pass skips the stencil plane's traffic.
enum MetalPassAttachment : uint32_t {
  kMetalPassAttachmentDepth = 1u << 0,
  kMetalPassAttachmentStencil = 1u << 1,
  kMetalPassAttachmentColor0 = 1u << 2,
};
constexpr uint32_t kMetalPassAttachmentColorShift = 2;
constexpr uint32_t kMetalPassAttachmentCount =
    kMetalPassAttachmentColorShift + xenos::kMaxColorRenderTargets;
constexpr uint32_t kMetalPassAttachmentAll =
    (1u << kMetalPassAttachmentCount) - 1u;
constexpr uint32_t kMetalPassAttachmentDepthStencil =
    kMetalPassAttachmentDepth | kMetalPassAttachmentStencil;

constexpr uint32_t MetalPassColorAttachmentBit(uint32_t index) {
  return 1u << (kMetalPassAttachmentColorShift + index);
}

struct MetalPassAttachmentUse {
  // Attachments whose existing contents a draw can read: blending that is
  // not an opaque replacement, a depth compare, a stencil compare.
  uint32_t reads = 0;
  // Attachments a draw can write: a nonzero color write mask, depth write
  // (or a pixel-shader depth export, which needs a depth attachment in the
  // pipeline even when the test is off), a stencil write mask with an op
  // that is not KEEP.
  uint32_t writes = 0;

  uint32_t used() const { return reads | writes; }
};

inline bool StencilFaceWrites(uint32_t write_mask, xenos::StencilOp fail,
                              xenos::StencilOp zfail, xenos::StencilOp zpass) {
  return write_mask != 0 &&
         (fail != xenos::StencilOp::kKeep || zfail != xenos::StencilOp::kKeep ||
          zpass != xenos::StencilOp::kKeep);
}

// `blend_controls` are the raw RB_BLENDCONTROL[0..3] values;
// `stencil_ref_mask` / `stencil_ref_mask_bf` the raw RB_STENCILREFMASK and
// RB_STENCILREFMASK_BF values. Logic ops (RB_COLORCONTROL rop3) are not
// implemented by any Xenia backend, so they do not enter the predicate.
inline MetalPassAttachmentUse GetMetalPassAttachmentUse(
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const uint32_t* blend_controls,
    uint32_t stencil_ref_mask, uint32_t stencil_ref_mask_bf,
    bool pixel_shader_exports_depth) {
  MetalPassAttachmentUse use;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    uint32_t nibble = (normalized_color_mask >> (4 * i)) & 0xFu;
    if (!nibble) {
      continue;
    }
    uint32_t bit = MetalPassColorAttachmentBit(i);
    use.writes |= bit;
    MetalColorAttachmentBlendState blend =
        GetMetalColorAttachmentBlendState(blend_controls[i], nibble);
    if (blend.blending_required) {
      use.reads |= bit;
    }
  }
  const reg::RB_DEPTHCONTROL dc = normalized_depth_control;
  if (dc.z_enable) {
    use.reads |= kMetalPassAttachmentDepth;
    if (dc.z_write_enable) {
      use.writes |= kMetalPassAttachmentDepth;
    }
  }
  if (pixel_shader_exports_depth) {
    // Hardware ignores oDepth when the test is off, but the host pipeline
    // for a depth-exporting fragment function must carry a depth attachment,
    // so the pass binds one. Counted as a write so the census does not call
    // the attachment avoidable.
    use.writes |= kMetalPassAttachmentDepth;
  }
  if (dc.stencil_enable) {
    use.reads |= kMetalPassAttachmentStencil;
    reg::RB_STENCILREFMASK srm;
    srm.value = stencil_ref_mask;
    bool writes = StencilFaceWrites(srm.stencilwritemask, dc.stencilfail,
                                    dc.stencilzfail, dc.stencilzpass);
    if (!writes && dc.backface_enable) {
      reg::RB_STENCILREFMASK srm_bf;
      srm_bf.value = stencil_ref_mask_bf;
      writes = StencilFaceWrites(srm_bf.stencilwritemask, dc.stencilfail_bf,
                                 dc.stencilzfail_bf, dc.stencilzpass_bf);
    }
    if (writes) {
      use.writes |= kMetalPassAttachmentStencil;
    }
  }
  return use;
}

// The pass-binding decision with hysteresis (design D2). `present` is the
// attachment set the render-target cache has accumulated for the current
// attachment key (what the legacy policy binds); `use` is this draw's use
// restricted to it. `open_bound` is the open pass's bound set when a pass
// with the same key is open, else 0 (`same_key_open` false).
//
// Returns the set the draw's pass must bind:
//  - the open pass's set when it already covers the draw (reuse);
//  - the union when the draw needs more (the open pass ends with reason
//    "attachment needed", the next one binds the union so an alternating
//    draw sequence splits once per transition, not per draw);
//  - the draw's own use when no pass with this key is open.
struct MetalPassBindingDecision {
  uint32_t bound = 0;
  bool reuse = false;   // the open pass continues
  bool split = false;   // the open pass ends for "attachment needed"
};

inline MetalPassBindingDecision DecideMetalPassBinding(uint32_t present,
                                                       uint32_t use,
                                                       bool same_key_open,
                                                       uint32_t open_bound) {
  MetalPassBindingDecision d;
  use &= present;
  if (!same_key_open) {
    d.bound = use;
    return d;
  }
  if ((use & ~open_bound) == 0) {
    d.bound = open_bound;
    d.reuse = true;
    return d;
  }
  d.bound = open_bound | use;
  d.split = true;
  return d;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PASS_ATTACHMENT_USE_H_
