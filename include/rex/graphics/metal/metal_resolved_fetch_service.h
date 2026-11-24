/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_RESOLVED_FETCH_SERVICE_H_
#define XENIA_GPU_METAL_METAL_RESOLVED_FETCH_SERVICE_H_

#include "rex/graphics/metal/metal_exact_cover_predicate.h"
#include "rex/graphics/metal/metal_scaled_cover_predicate.h"

namespace rex {
namespace graphics {
namespace metal {

// The ONE service classification for a texture fetch whose base address may
// be a registered resolve target (OpenSpec gpu-texture-loads slice 5R,
// design: docs/dev/archive/evidence/metal_resolve_backed_texture_load_design.md). It composes
// the two EXISTING production predicates in GetTextureSlot's serve order —
// exact-cover first, then the bridge condition — so every consumer (the
// GetTextureSlot serve chain, the routed-load prepass, and any diagnostic)
// shares one decision and the guards are never duplicated.
enum class ResolvedFetchService {
  // The fetch base is not a registered resolve target: the ordinary
  // guest-memory path (cache/untile) applies.
  kNotResolveBacked,
  // Served GPU-side from the registered snapshot via the bridge condition
  // (depth, color-bridge A/B mode, never-RAM-backed dests, or the covered
  // R32F fast path incl. its derived 1x1 tip view). Never reads RAM.
  kBridgeSnapshot,
  // Served GPU-side from the snapshot via the exact-cover fast path
  // (ExactCoverSnapshotEligible: RAM-backed + newest writeback full-rect +
  // exact shape/pitch/endian + unsigned RGBA8). Never reads RAM.
  kExactCoverSnapshot,
  // RAM is authoritative for this fetch: today the CPU untile serves it;
  // under slice 5R it is the ONLY service class eligible for the GPU
  // load-shader route.
  kRamContent,
  // Draw resolution scaling only: a fetch that would otherwise be kRamContent,
  // served GPU-side from the snapshot because its guest RAM holds exactly the
  // snapshot's representative pixels (ScaledCoverSnapshotEligible). Never
  // reads RAM. Never selected at 1x.
  kScaledCoverSnapshot,
};

struct ResolvedFetchServiceInputs {
  bool has_resolved_target = false;
  // Built exactly the way GetTextureSlot builds it today (including the
  // stage-3 / color-bridge statics and the snapshot pixel-format checks).
  ExactCoverInputs exact = {};
  // The covered-R32F fast-path condition, computed by the caller exactly as
  // the serve chain computes it (incl. the XE_METAL_TIP_RAM_FETCH override).
  bool covered_r32f = false;
  // Scaled-cover inputs (scaled_session is false at 1x).
  ScaledCoverInputs scaled = {};
  // The registered target is a depth snapshot smaller than the fetch, and the
  // rest of the fetch's guest extent holds other resolves' content (computed
  // by the caller from the registry).
  bool depth_fetch_composite = false;
};

inline ResolvedFetchService ClassifyResolvedFetch(
    const ResolvedFetchServiceInputs& in) {
  if (!in.has_resolved_target) {
    return ResolvedFetchService::kNotResolveBacked;
  }
  // Serve order mirrors GetTextureSlot: exact-cover is evaluated first.
  if (ExactCoverSnapshotEligible(in.exact)) {
    return ResolvedFetchService::kExactCoverSnapshot;
  }
  // A depth snapshot holds the depth as a float view; it can only stand in for
  // a fetch that reads the destination AS depth (k_24_8 / k_24_8_FLOAT). A
  // fetch of the same range through any other format wants the raw D24S8
  // words (GTA IV restores its depth/stencil buffer by drawing the saved
  // k_24_8_FLOAT resolve back over EDRAM as k_8_8_8_8 colour; the float view
  // gave (1,0,0,1) per texel, the depth buffer became all-ones and every
  // deferred light failed its depth test). The depth writeback keeps the raw
  // words in RAM, so RAM serves those fetches.
  if (in.exact.rt_is_depth &&
      in.exact.fetch_format != xenos::TextureFormat::k_24_8 &&
      in.exact.fetch_format != xenos::TextureFormat::k_24_8_FLOAT) {
    return ResolvedFetchService::kRamContent;
  }
  // One snapshot can stand in for a larger fetch only while the rest of the
  // fetch holds nothing (the fitted view zero-fills it). Red Dead Redemption:
  // Undead Nightmare resolves a 128x128 scratch depth map to the base of its
  // 1600x1600 cascade atlas, whose 800x800 cascades are resolves of their own
  // at other bases; the scratch snapshot fitted to the atlas read every
  // cascade as depth 0 and the deferred shadow pass darkened whatever it drew
  // while that registration was live. The depth writeback keeps every resolve
  // in RAM, so RAM serves the composition.
  if (in.exact.rt_is_depth && in.depth_fetch_composite) {
    return ResolvedFetchService::kRamContent;
  }
  if (in.exact.rt_is_depth || in.exact.color_bridge || !in.exact.ram_backed ||
      in.covered_r32f) {
    return ResolvedFetchService::kBridgeSnapshot;
  }
  if (ScaledCoverSnapshotEligible(in.scaled)) {
    return ResolvedFetchService::kScaledCoverSnapshot;
  }
  return ResolvedFetchService::kRamContent;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_RESOLVED_FETCH_SERVICE_H_
