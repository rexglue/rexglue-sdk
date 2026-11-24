/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_FETCH_OVERLAP_H_
#define XENIA_GPU_METAL_METAL_FETCH_OVERLAP_H_

#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

// A fetched guest texture occupies TWO disjoint guest storage ranges — the
// base level(s) at base_address and the mip chain at mip_address — which live
// at unrelated guest addresses (mip_address is a separate pointer, not
// base+size). The pre-draw pending-writeback check must treat them as two
// ranges: a writeback overlaps the fetch iff it intersects EITHER range, and
// the gap between them is NOT covered. The previous conservative extent
// (pitch*height*4 from the base) spanned that gap, spuriously flushing for
// unrelated resolves in adjacent memory (e.g. a 512x512 DXT texture whose
// 1 MB conservative span aliased a neighbouring uncompressed pyramid).
struct FetchGuestRanges {
  uint32_t base_begin = 0;
  uint32_t base_end = 0;
  // Empty (begin == end) when the fetch has no separate mip storage.
  uint32_t mip_begin = 0;
  uint32_t mip_end = 0;
};

// Half-open [a_begin, a_end) intersects [b_begin, b_end).
inline bool GuestRangesIntersect(uint32_t a_begin, uint32_t a_end,
                                 uint32_t b_begin, uint32_t b_end) {
  return a_begin < b_end && b_begin < a_end;
}

// True iff a pending writeback [wb_begin, wb_end) intersects the base range or
// the (present) mip range. Never the gap between them.
inline bool WritebackOverlapsFetch(const FetchGuestRanges& r, uint32_t wb_begin,
                                   uint32_t wb_end) {
  if (GuestRangesIntersect(r.base_begin, r.base_end, wb_begin, wb_end)) {
    return true;
  }
  if (r.mip_end > r.mip_begin &&
      GuestRangesIntersect(r.mip_begin, r.mip_end, wb_begin, wb_end)) {
    return true;
  }
  return false;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_FETCH_OVERLAP_H_
