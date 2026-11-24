/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_RENDER_TARGET_TRANSFER_CLASS_H_
#define XENIA_GPU_RENDER_TARGET_TRANSFER_CLASS_H_

#include <cstdint>

#include "rex/graphics/xenos.h"

namespace rex {
namespace graphics {

// The kind of ownership transition an EDRAM tile range undergoes when a render
// target reclaims tiles a different render target last owned. This is the
// backend-agnostic classification of the Metal PerformTransfers cascade
// (metal_render_target_cache.mm) extracted so it can be validated independently
// of a live device (the L2/L3b conformance rung) and recorded into the
// per-transition manifest (the ownership-transition stream).
//
// IMPORTANT: The ordering and exact predicates here MUST mirror the live
// cascade byte-for-byte. The live code calls ClassifyTransfer() to drive the
// branch it takes AND to label the manifest, so any semantic drift between this
// function and the executed transfer is a test failure by construction.
enum class TransferKind : uint32_t {
  // No handler matched (e.g. a 64bpp owner, or mismatched depth host formats):
  // the destination keeps its own previous content ("key-local persistence").
  kSkip = 0,
  // Same host pixel format AND same MSAA pixel-block size: a straight
  // tile-by-tile blit, no reformatting.
  kBlit,
  // Same host pixel format, different MSAA sample density. LEGACY behavior:
  // remap the differently sized per-tile pixel blocks through a sampling pass
  // that COLLAPSES/synthesizes missing samples when up- or down-sampling.
  // CANONICAL bypass (ShouldUseCanonicalTransferPath): the physical EDRAM words
  // are preserved verbatim, so no missing-sample synthesis happens; the
  // classification is still kCrossMsaa so the manifest records the handoff.
  kCrossMsaa,
  // Raw-word color reinterpreted as depth: unpack the 24-bit guest depth and
  // write host depth via a depth-only pass.
  kColorToDepth,
  // Depth reinterpreted as raw-word color. LEGACY behavior: the default SKIPS
  // the GPU work here (d2c skip/zero, key-local persistence) unless
  // XE_METAL_D2C_REAL or a same-frame ping-pong return, so no bits are
  // transported. CANONICAL bypass (ShouldUseCanonicalTransferPath): the real
  // physical EDRAM words are transported through the scheduler, so the d2c
  // skip/zero policy no longer applies. Either way the CLASSIFICATION is still
  // kDepthToColor so the manifest records the ownership handoff.
  kDepthToColor,
  // Color<->color 32bpp reinterpret where only the host container differs
  // (raw-word 8888 bytes vs R32F bits): identical EDRAM word, no math.
  kColorReinterpret,
  // General 32bpp color->color transfer (handles reformat + cross-MSAA in one
  // pass): the catch-all for color combos the specific handlers don't cover
  // (e.g. HDR 7e3-packed RGBA16F <-> 8888).
  kColorConvert,
};

// The observable attributes of a (source, destination) render-target pair that
// determine which transfer branch runs. Every field is derivable from the two
// RenderTargetKeys plus the backend's host-format choice for each; the caller
// fills them from live state and the conformance test fills them synthetically.
struct TransferClassInput {
  // From RenderTargetKey.is_depth.
  bool src_is_depth = false;
  bool dst_is_depth = false;
  // From RenderTargetKey::Is64bpp().
  bool src_is_64bpp = false;
  bool dst_is_64bpp = false;
  // From RenderTargetKey.msaa_samples (xenos::MSAASamples enum value).
  uint32_t src_msaa = 0;
  uint32_t dst_msaa = 0;
  // source->pixel_format() == dest->pixel_format() (host MTLPixelFormat equal).
  // Kept as an explicit input because two distinct host formats can share a
  // category (e.g. two depth formats) and must NOT be treated as blit-equal.
  bool host_format_equal = false;
  // source/dest host pixel_format() == the backend's raw-word color format
  // (Metal kColorFormat, an 8888 UNorm).
  bool src_is_color8888 = false;
  bool dst_is_color8888 = false;
  // source/dest host pixel_format() == the 32bpp float reinterpret container
  // (Metal R32Float).
  bool src_is_r32f = false;
  bool dst_is_r32f = false;
};

// GetMsaaShifts, inlined: the per-tile pixel block is halved in X at 4x and in
// Y at 2x/4x. Mirrors GetMsaaShifts() in metal_render_target_cache.mm.
inline void TransferMsaaShifts(uint32_t msaa_samples, uint32_t& x_shift,
                               uint32_t& y_shift) {
  x_shift = msaa_samples >= uint32_t(xenos::MSAASamples::k4X) ? 1 : 0;
  y_shift = msaa_samples >= uint32_t(xenos::MSAASamples::k2X) ? 1 : 0;
}

// Classify a single tile-range transfer. Mirrors the PerformTransfers cascade
// exactly, in the same order (order matters: earlier branches win).
inline TransferKind ClassifyTransfer(const TransferClassInput& in) {
  uint32_t sxs, sys, dxs, dys;
  TransferMsaaShifts(in.src_msaa, sxs, sys);
  TransferMsaaShifts(in.dst_msaa, dxs, dys);

  if (in.host_format_equal && sxs == dxs && sys == dys) {
    return TransferKind::kBlit;
  }
  if (in.host_format_equal && in.src_is_depth == in.dst_is_depth &&
      !in.src_is_64bpp && !in.dst_is_64bpp) {
    return TransferKind::kCrossMsaa;
  }
  if (in.dst_is_depth && !in.src_is_depth && in.src_is_color8888) {
    return TransferKind::kColorToDepth;
  }
  if (!in.dst_is_depth && in.src_is_depth && in.dst_is_color8888) {
    return TransferKind::kDepthToColor;
  }
  if (!in.src_is_depth && !in.dst_is_depth && !in.src_is_64bpp &&
      !in.dst_is_64bpp &&
      ((in.src_is_color8888 && in.dst_is_r32f) ||
       (in.src_is_r32f && in.dst_is_color8888))) {
    return TransferKind::kColorReinterpret;
  }
  if (!in.src_is_depth && !in.dst_is_depth && !in.src_is_64bpp &&
      !in.dst_is_64bpp) {
    return TransferKind::kColorConvert;
  }
  return TransferKind::kSkip;
}

// Decide whether an ownership transition takes the canonical sample-accurate
// EDRAM path instead of any legacy per-branch transfer handler.
//
// When this returns true the live cascade materializes/packs the physical EDRAM
// words through the canonical scheduler and SKIPS every legacy transfer policy,
// including:
//   * the kDepthToColor "d2c skip/zero" default (key-local persistence that
//     transports no bits unless XE_METAL_D2C_REAL / a same-frame ping-pong),
//   * the kCrossMsaa collapsed missing-sample synthesis (legacy sampling pass
//     that fabricates absent samples when remapping differently sized blocks).
// The canonical path preserves the real physical words across the handoff, so
// those legacy approximations no longer apply.
//
// All three preconditions must hold: canonical raster must be enabled, and both
// the source and destination formats must be representable in canonical EDRAM.
// Any false input falls back to the legacy branch cascade.
inline bool ShouldUseCanonicalTransferPath(bool canonical_raster_enabled,
                                           bool source_format_supported,
                                           bool destination_format_supported) {
  return canonical_raster_enabled && source_format_supported &&
         destination_format_supported;
}

inline const char* TransferKindName(TransferKind kind) {
  switch (kind) {
    case TransferKind::kSkip:
      return "skip";
    case TransferKind::kBlit:
      return "blit";
    case TransferKind::kCrossMsaa:
      return "cross_msaa";
    case TransferKind::kColorToDepth:
      return "color_to_depth";
    case TransferKind::kDepthToColor:
      return "depth_to_color";
    case TransferKind::kColorReinterpret:
      return "color_reinterpret";
    case TransferKind::kColorConvert:
      return "color_convert";
  }
  return "?";
}

}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_RENDER_TARGET_TRANSFER_CLASS_H_
