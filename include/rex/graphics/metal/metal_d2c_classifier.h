/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_D2C_CLASSIFIER_H_
#define XENIA_GPU_METAL_METAL_D2C_CLASSIFIER_H_

// Stage C (implement-metal-depth-semantics 4.1/4.2): the depth->color
// consumer classifier, as a PURE, device-free type.
//
// WHAT IT DECIDES: cascade branch 5 receives a depth->color ownership
// transfer and must choose between two consumer semantics:
//   kKeyLocalPersistence — the color key relies on its own previous content
//     (the historic default: skip the transfer, keep key-local EDRAM);
//   kRealTransfer — the color consumer decodes the depth RT's actual bits
//     (Sonic's DoF packs motion vectors as depth over the blur buffer's
//     tiles; skipping leaves stale scene pixels to be decoded as gather
//     vectors — the melted-mip / white-screen defect).
//
// IDENTITY RULES (review-mandated): lineage is keyed by STABLE
// RenderTargetKey values, exact tile ranges, and transition order — NEVER
// RenderTarget* (the cache-clear code warns that RT addresses recycle; the
// old pointer-keyed same-frame tracker carried that hazard). A return
// classifies as real only against an EXACT, UNSUPERSEDED color->depth
// lineage: same color key, same depth key, and no later conflicting record
// overlapping the claimed tiles. Partial overlaps SPLIT: only the proven
// intersection is real; everything else stays key-local persistence.
//
// CROSS-FRAME: representable (records carry the swap index) but DISABLED by
// default. Same-frame Sonic proves the classifier seam, not cross-frame
// engagement — cross-frame candidates are MEASURED (CountCrossFrameCandidates
// feeds a diagnostic counter) before any enablement decision.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace rex {
namespace graphics {
namespace metal {

// One color->depth lend: `color_key`'s tiles [start,end) were transferred
// into `depth_key` at (swap, sequence). sequence is the global transition
// counter — total order across all transfers, the supersession authority.
struct MetalD2cLineageRecord {
  uint32_t color_key;
  uint32_t depth_key;
  uint32_t start_tiles;
  uint32_t end_tiles;
  uint64_t swap;
  uint64_t sequence;
};

enum class MetalD2cVerdict : uint32_t {
  kKeyLocalPersistence = 0,
  kRealTransfer,
};

// A classified sub-range of the queried transfer. Reason tokens are stable
// strings for the transition manifest.
struct MetalD2cSegment {
  uint32_t start_tiles;
  uint32_t end_tiles;
  MetalD2cVerdict verdict;
  const char* reason;
};

class MetalD2cLineage {
 public:
  // Records a color->depth transfer. Any OLDER record whose tile range
  // overlaps this one is superseded on the overlap: ownership of those tiles
  // moved again, so an earlier lend can no longer prove a return. Trimming
  // (rather than deleting) preserves the non-overlapping remainder.
  void RecordColorToDepth(uint32_t color_key, uint32_t depth_key,
                          uint32_t start_tiles, uint32_t end_tiles,
                          uint64_t swap, uint64_t sequence) {
    Supersede(start_tiles, end_tiles);
    records_.push_back(
        {color_key, depth_key, start_tiles, end_tiles, swap, sequence});
  }

  // A conflicting overlapping transfer that is NOT a color->depth lend (any
  // other ownership movement over these tiles) invalidates lineage there.
  void SupersedeRange(uint32_t start_tiles, uint32_t end_tiles) {
    Supersede(start_tiles, end_tiles);
  }

  // Cache/title clear, trace restore, failed publication: nothing recorded
  // before this point may prove a return.
  void InvalidateAll() { records_.clear(); }

  // Classifies a depth->color transfer (depth_key -> color_key over
  // [start,end) at current_swap). Returns ordered, disjoint segments
  // covering exactly [start,end).
  std::vector<MetalD2cSegment> Classify(uint32_t depth_key,
                                        uint32_t color_key,
                                        uint32_t start_tiles,
                                        uint32_t end_tiles,
                                        uint64_t current_swap,
                                        bool allow_cross_frame) const {
    // Collect proven intersections: exact key pair, unsuperseded (records_
    // already maintains that invariant), frame rule per the gate.
    std::vector<std::pair<uint32_t, uint32_t>> proven;
    for (const MetalD2cLineageRecord& record : records_) {
      if (record.color_key != color_key || record.depth_key != depth_key) {
        continue;
      }
      if (!allow_cross_frame && record.swap != current_swap) {
        continue;
      }
      uint32_t s = std::max(record.start_tiles, start_tiles);
      uint32_t e = std::min(record.end_tiles, end_tiles);
      if (s < e) {
        proven.emplace_back(s, e);
      }
    }
    std::sort(proven.begin(), proven.end());
    // Merge and emit segments over [start,end).
    std::vector<MetalD2cSegment> out;
    uint32_t cursor = start_tiles;
    for (const auto& range : proven) {
      uint32_t s = std::max(range.first, cursor);
      uint32_t e = range.second;
      if (e <= cursor) {
        continue;  // swallowed by an earlier proven segment
      }
      if (s > cursor) {
        out.push_back({cursor, s, MetalD2cVerdict::kKeyLocalPersistence,
                       "no-lineage"});
      }
      out.push_back({s, e, MetalD2cVerdict::kRealTransfer,
                     "pingpong-return"});
      cursor = e;
    }
    if (cursor < end_tiles) {
      out.push_back({cursor, end_tiles,
                     MetalD2cVerdict::kKeyLocalPersistence, "no-lineage"});
    }
    return out;
  }

  // Measurement (review-mandated before any cross-frame enablement): tiles
  // of [start,end) that WOULD classify real cross-frame but not same-frame.
  uint32_t CountCrossFrameCandidateTiles(uint32_t depth_key,
                                         uint32_t color_key,
                                         uint32_t start_tiles,
                                         uint32_t end_tiles,
                                         uint64_t current_swap) const {
    uint32_t same = 0, cross = 0;
    for (const MetalD2cSegment& seg :
         Classify(depth_key, color_key, start_tiles, end_tiles, current_swap,
                  /*allow_cross_frame=*/false)) {
      if (seg.verdict == MetalD2cVerdict::kRealTransfer) {
        same += seg.end_tiles - seg.start_tiles;
      }
    }
    for (const MetalD2cSegment& seg :
         Classify(depth_key, color_key, start_tiles, end_tiles, current_swap,
                  /*allow_cross_frame=*/true)) {
      if (seg.verdict == MetalD2cVerdict::kRealTransfer) {
        cross += seg.end_tiles - seg.start_tiles;
      }
    }
    return cross - same;
  }

  size_t record_count() const { return records_.size(); }

 private:
  void Supersede(uint32_t start_tiles, uint32_t end_tiles) {
    std::vector<MetalD2cLineageRecord> kept;
    kept.reserve(records_.size() + 1);
    for (const MetalD2cLineageRecord& record : records_) {
      if (record.end_tiles <= start_tiles ||
          record.start_tiles >= end_tiles) {
        kept.push_back(record);
        continue;
      }
      // Trim the overlap; keep non-overlapping remainders.
      if (record.start_tiles < start_tiles) {
        MetalD2cLineageRecord left = record;
        left.end_tiles = start_tiles;
        kept.push_back(left);
      }
      if (record.end_tiles > end_tiles) {
        MetalD2cLineageRecord right = record;
        right.start_tiles = end_tiles;
        kept.push_back(right);
      }
    }
    records_.swap(kept);
  }

  std::vector<MetalD2cLineageRecord> records_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_D2C_CLASSIFIER_H_
