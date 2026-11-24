/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SCALED_COVER_PREDICATE_H_
#define XENIA_GPU_METAL_METAL_SCALED_COVER_PREDICATE_H_

#include <cstdint>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// EXPAND uses the same half-float texels and signed view as FLOAT. Keep this
// alias decision shared by snapshot matching and signed-float eligibility.
inline bool ScaledCoverRGBA16FloatFormat(xenos::TextureFormat format,
                                       bool expand_enabled) {
  return format == xenos::TextureFormat::k_16_16_16_16_FLOAT ||
         (expand_enabled &&
          format == xenos::TextureFormat::k_16_16_16_16_EXPAND);
}

// Scaled-cover serving (implement-metal-draw-resolution-scaling): at a draw
// resolution scale, a RAM-backed color resolve's guest RAM holds the
// representative pixels of its snapshot, so while the registration is live
// (no later writer has overlapped its watched guest range, which retires it)
// the snapshot is the same content at higher resolution. This predicate decides
// when a fetch that would read that RAM may read the snapshot instead: the
// fetch must sample exactly the snapshot's surface (shape, pitch, no mips,
// format representation, endian), and every row it reads must have been
// written by resolves since the snapshot texture was created. Device-free so
// the decision is unit-testable; evaluated only inside ClassifyResolvedFetch.
struct ScaledCoverInputs {
  bool scaled_session = false;
  bool rt_is_depth = false;
  bool ram_backed = false;
  bool is_signed = false;
  // The snapshot's pixel format holds the fetch format's texel values (same
  // representation the RAM upload decodes to).
  bool snapshot_format_matches_fetch = false;
  bool fetch_tiled = false;
  xenos::DataDimension fetch_dimension = xenos::DataDimension::k1D;
  bool fetch_stacked = false;
  uint32_t fetch_mip_min_level = 0;
  uint32_t fetch_mip_max_level = 0;
  uint32_t fetch_width = 0;
  uint32_t fetch_height = 0;
  uint32_t fetch_pitch_texels = 0;
  // The snapshot texture's extent in guest pixels.
  uint32_t snapshot_width = 0;
  uint32_t snapshot_height = 0;
  // Guest rows from the top of the snapshot written by resolves (the base
  // resolve and joined strips) since the texture was created.
  uint32_t rows_written = 0;
  uint32_t fetch_endianness = 0;
  uint32_t wb_endian = 0;
  // Not a condition: which snapshot the inputs describe, the base's own
  // registration or the column assembly at the base (serve routing only).
  bool from_assembly = false;
};

inline bool ScaledCoverSnapshotEligible(const ScaledCoverInputs& in) {
  return in.scaled_session && !in.rt_is_depth && in.ram_backed &&
         !in.is_signed && in.snapshot_format_matches_fetch && in.fetch_tiled &&
         in.fetch_dimension == xenos::DataDimension::k2DOrStacked &&
         !in.fetch_stacked && in.fetch_mip_min_level == 0 &&
         in.fetch_mip_max_level == 0 &&
         in.fetch_width == in.snapshot_width &&
         in.fetch_height == in.snapshot_height &&
         in.fetch_pitch_texels == in.snapshot_width &&
         in.rows_written >= in.fetch_height &&
         in.fetch_endianness == in.wb_endian;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SCALED_COVER_PREDICATE_H_
