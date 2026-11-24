/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_EXACT_COVER_PREDICATE_H_
#define XENIA_GPU_METAL_METAL_EXACT_COVER_PREDICATE_H_

#include <cstdint>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Pure value form of the Stage-3 exact-cover reader decision (the branch in
// MetalTextureCache::GetTextureSlot that serves a resolve snapshot GPU-side
// instead of re-uploading its guest RAM). Extracted as a device-free
// predicate so the safety invariant is unit-testable without a Metal device;
// the reader builds this struct and calls ExactCoverSnapshotEligible so the
// production path and the tests share ONE decision.
//
// The snapshot is served only when it is provably the same content the RAM
// upload would produce. Each field guards a distinct way they could differ;
// see the call site for the per-condition rationale.
struct ExactCoverInputs {
  bool rt_is_depth;
  bool color_bridge;             // XE_METAL_COLOR_BRIDGE / stage3-off A/B state
  bool ram_backed;               // a writeback at this base has run
  bool full_rect;                // newest writeback covered the full extent
  bool is_signed;                // signed fetches need an SNORM view, not this
  bool snapshot_is_rgba8unorm;   // snapshot is the 8-bit-unorm surface
  xenos::TextureFormat fetch_format;
  bool fetch_tiled;
  xenos::DataDimension fetch_dimension;
  bool fetch_stacked;
  uint32_t fetch_mip_min_level;
  uint32_t fetch_mip_max_level;
  uint32_t fetch_width;         // fetch extent, texels
  uint32_t fetch_height;
  uint32_t fetch_pitch_texels;  // fetch.pitch << 5
  uint32_t rt_width;            // registered snapshot / writeback extent
  uint32_t rt_height;
  uint32_t fetch_endianness;
  uint32_t wb_endian;           // endian the writeback scattered with
};

inline bool ExactCoverSnapshotEligible(const ExactCoverInputs& in) {
  return !in.rt_is_depth && !in.color_bridge && in.ram_backed &&
         in.full_rect && !in.is_signed && in.snapshot_is_rgba8unorm &&
         in.fetch_format == xenos::TextureFormat::k_8_8_8_8 &&
         in.fetch_tiled &&
         in.fetch_dimension == xenos::DataDimension::k2DOrStacked &&
         !in.fetch_stacked && in.fetch_mip_min_level == 0 &&
         in.fetch_mip_max_level == 0 && in.fetch_width == in.rt_width &&
         in.fetch_height == in.rt_height &&
         in.fetch_pitch_texels == in.rt_width &&
         in.fetch_endianness == in.wb_endian;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_EXACT_COVER_PREDICATE_H_
