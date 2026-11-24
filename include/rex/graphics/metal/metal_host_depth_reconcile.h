/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_HOST_DEPTH_RECONCILE_H_
#define XENIA_GPU_METAL_METAL_HOST_DEPTH_RECONCILE_H_

// CPU oracle for Stage B two-source depth reconciliation
// (implement-metal-depth-semantics 3.3). Mirrors the MSL
// xe_host_depth_reconcile in metal_edram_msl.h; the L1 kernel case pins the
// two against each other, and this header is the L0 truth the kernel must
// match.
//
// CONTRACT (the reviewer-specified rule): the canonical guest 20e4 word is
// the authority. The host-precision float is used IF AND ONLY IF packing it
// (under the arm's rounding mode) reproduces the guest word's depth bits —
// i.e. the guest value is provably just the rounded shadow of this exact
// host value. Any mismatch — the guest wrote the range, another RT's pack
// produced the word, the lane is stale — makes the guest word win. Validity
// is pointwise; no ownership metadata exists to go stale.
//
// FLOAT24-ONLY by construction: the host-depth predicate
// (MetalHostDepthEncodingDifferent) is true only for kD24FS8. unorm24 host
// round-trips exactly within its precision, so kD24S8 never reaches
// reconciliation. Host domain is the depth attachment's [0,1] encoding of
// the guest [0,2) 20e4 range (the canonical *0.5 mapping).
//
// Stencil is NOT reconciled: the guest word's stencil byte is always
// authoritative (the host lane stores no stencil).

#include <cstdint>
#include <vector>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Packs a host-domain depth value to guest 20e4 bits under the arm's
// rounding mode. *2.0f is exact (power of two); saturation handled by
// Float32To20e4.
inline uint32_t MetalHostDepthPack(float host_depth,
                                   bool round_to_nearest_even) {
  return xenos::Float32To20e4(host_depth * 2.0f, round_to_nearest_even);
}

// Depth-to-depth host precision (2026-09-17, Red Dead speckle). The shared
// planner attaches a separate host-depth source to a transfer only when it
// differs from the transfer source; a float24 depth target re-bound at another
// MSAA count (or another key of the same encoding) therefore arrives as a bare
// depth->depth transfer. The reference performs that transfer as a direct
// host-float copy. Our canonical path packs the source to 20e4 words and
// materializes the destination from them, so the destination would hold the
// quantized value and every EQUAL/GEQUAL test of the following pass against
// the exact value would fail. Pure decision: such a transfer carries the
// transfer source itself as the host-precision source (same in-plan lane
// refresh, same pointwise predicate). unorm24 round-trips exactly and is
// excluded; a planner-provided source always takes precedence.
inline bool MetalDepthToDepthCarriesHostPrecision(
    bool lane_enabled, bool has_planner_host_source, bool src_is_depth,
    bool dst_is_depth, xenos::DepthRenderTargetFormat src_format,
    xenos::DepthRenderTargetFormat dst_format) {
  return lane_enabled && !has_planner_host_source && src_is_depth &&
         dst_is_depth &&
         src_format == xenos::DepthRenderTargetFormat::kD24FS8 &&
         dst_format == xenos::DepthRenderTargetFormat::kD24FS8;
}

// The provenance-complete two-source join record: the shared plan's FULL
// identity — destination attachment, SELECTED host source, exact tile range.
// Reducing this to bare ranges was the 8551 regression's shape (a different
// destination or source sharing tiles in the batch could consume the wrong
// lane provenance); the selection below keys on all three.
struct MetalTwoSourceRecord {
  uint32_t dest_key;        // RenderTargetKey.key of the transfer destination
  const void* host_source;  // RenderTarget* identity of the selected source
  uint32_t start_tiles;
  uint32_t end_tiles;
};

// Pure selection: the EXACT intersections of one canonical materialize
// operation [op_start, op_end) for `dest_key` with the batch's two-source
// records. Only matching destinations select; results carry the source
// identity and are clipped to the intersection — reconciliation must never
// exceed it.
inline std::vector<MetalTwoSourceRecord> MetalSelectTwoSourceIntersections(
    const std::vector<MetalTwoSourceRecord>& records, uint32_t dest_key,
    uint32_t op_start, uint32_t op_end) {
  std::vector<MetalTwoSourceRecord> out;
  for (const MetalTwoSourceRecord& record : records) {
    if (record.dest_key != dest_key) {
      continue;
    }
    uint32_t start = record.start_tiles > op_start ? record.start_tiles
                                                   : op_start;
    uint32_t end = record.end_tiles < op_end ? record.end_tiles : op_end;
    if (start < end) {
      out.push_back({record.dest_key, record.host_source, start, end});
    }
  }
  return out;
}

// Disjointness pin: the executor runs ALL lane refreshes before ALL
// reconcile draws, so two overlapping same-destination selections would let
// the LAST refresh win both draws. The shared planner walks disjoint
// ownership ranges per destination, so overlap is an invariant violation;
// this drops every record overlapping an EARLIER one (fail closed: the
// dropped range materializes pure guest words) and reports whether anything
// was dropped so the caller can scream.
inline bool MetalDropOverlappingTwoSourceRecords(
    std::vector<MetalTwoSourceRecord>* records) {
  bool dropped = false;
  for (size_t i = 0; i < records->size(); ++i) {
    for (size_t j = i + 1; j < records->size();) {
      const MetalTwoSourceRecord& a = (*records)[i];
      const MetalTwoSourceRecord& b = (*records)[j];
      if (a.start_tiles < b.end_tiles && b.start_tiles < a.end_tiles) {
        records->erase(records->begin() + j);
        dropped = true;
      } else {
        ++j;
      }
    }
  }
  return dropped;
}

// The reconciled host-domain depth for one sample.
inline float MetalHostDepthReconcile(uint32_t guest_word, float host_depth,
                                     bool round_to_nearest_even) {
  uint32_t guest_d24 = guest_word >> 8;
  if (MetalHostDepthPack(host_depth, round_to_nearest_even) == guest_d24) {
    return host_depth;  // bit-exact host preservation, not a re-decode
  }
  return xenos::Float20e4To32(guest_d24) * 0.5f;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_HOST_DEPTH_RECONCILE_H_
