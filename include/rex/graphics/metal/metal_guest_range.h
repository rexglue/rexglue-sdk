/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_GUEST_RANGE_H_
#define XENIA_GPU_METAL_METAL_GUEST_RANGE_H_

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <rex/graphics/metal/metal_shared_memory.h>

namespace rex {
namespace graphics {
namespace metal {

// One normalized, overflow-safe, half-open guest physical range for the cross-
// writer memory-authority contract (OpenSpec
// implement-metal-cross-writer-memory-authority, task 2.1).
//
// This is a pure value type: device-free, non-owning, and holding no authority,
// version, or pending-producer state. It exists so writer records, overlap
// checks, selective drains, consumer preparation, and audit telemetry all speak
// one range shape instead of the raw 32-bit end arithmetic scattered across the
// command processor today (which cannot establish wrap/boundary correctness --
// see docs/dev/archive/evidence/metal_cross_writer_memory_authority_audit.md
// blocker 7).
//
// Invariants of a constructed GuestRange:
//   * non-empty: begin < end;
//   * in bounds: end <= kGuestPhysicalSize (the fixed 512 MiB domain the Metal
//     shared-memory buffer spans, buffer offset == guest physical byte address);
//   * overflow-safe: the end is computed in 64-bit and clamped, so a base+length
//     that would wrap 32-bit yields the in-bounds interval, never a wrapped one.
//
// The factories return std::nullopt for input that has no representable in-bounds
// byte (zero length, or a base at/after the end of memory). Callers treat nullopt
// as "no range to prepare / no dependency", never as an empty-but-present range.
class GuestRange {
 public:
  // The fixed guest physical domain. The Metal shared-memory buffer is a single
  // 512 MiB shared-storage MTLBuffer (::rex::gpu::SharedMemory::kBufferSize == 1 << 29 ==
  // 0x20000000) with buffer offset == guest physical address, so this is also
  // the exact clamp boundary for every authority range.
  static constexpr uint64_t kGuestPhysicalSize =
      static_cast<uint64_t>(::rex::gpu::SharedMemory::kBufferSize);

  // How two ranges relate. Adjacency (touching at a shared endpoint) is a
  // DISTINCT category from overlap: adjacent ranges share zero bytes and MUST
  // NOT trigger a producer drain. Disjoint means neither overlapping nor
  // touching (a real gap between them).
  enum class Relation {
    kDisjoint,     // no shared bytes and not touching (gap > 0)
    kAdjacent,     // touch at exactly one endpoint; no shared bytes
    kPartial,      // share bytes; neither fully contains the other
    kExact,        // identical [begin, end)
    kContains,     // this fully contains other (and is larger)
    kContainedBy,  // other fully contains this (and is larger)
  };

  // Build from a guest base and byte length. Length is 32-bit; the end is
  // computed in 64-bit and clamped to kGuestPhysicalSize. Returns nullopt when
  // length == 0 or base >= kGuestPhysicalSize (no in-bounds byte exists).
  static std::optional<GuestRange> FromBaseLength(uint32_t base,
                                                  uint32_t length) {
    if (length == 0) {
      return std::nullopt;
    }
    return FromBounds(static_cast<uint64_t>(base),
                      static_cast<uint64_t>(base) + static_cast<uint64_t>(length));
  }

  // Build only if the complete requested interval is in the guest-physical
  // domain. This is for host dereferences that will read the exact byte count;
  // unlike authority invalidations, they must reject rather than clamp.
  static std::optional<GuestRange> FromExactBaseLength(uint32_t base,
                                                       uint32_t length) {
    std::optional<GuestRange> range = FromBaseLength(base, length);
    if (!range || range->start() != base || range->length() != length) {
      return std::nullopt;
    }
    return range;
  }

  // Build from an explicit half-open [begin, end) computed in 64-bit. The end
  // is clamped to kGuestPhysicalSize. Returns nullopt when the clamped interval
  // is empty (end <= begin after clamping, or begin >= kGuestPhysicalSize).
  static std::optional<GuestRange> FromBounds(uint64_t begin, uint64_t end) {
    if (begin >= kGuestPhysicalSize) {
      return std::nullopt;
    }
    uint64_t clamped_end = std::min(end, kGuestPhysicalSize);
    if (clamped_end <= begin) {
      return std::nullopt;
    }
    return GuestRange(static_cast<uint32_t>(begin),
                      static_cast<uint32_t>(clamped_end));
  }

  uint32_t start() const { return begin_; }
  uint32_t end() const { return end_; }  // half-open; may equal kGuestPhysicalSize
  uint32_t length() const { return end_ - begin_; }

  // Share at least one byte.
  bool Overlaps(const GuestRange& o) const {
    return begin_ < o.end_ && o.begin_ < end_;
  }
  // Every byte of o is inside this range.
  bool Contains(const GuestRange& o) const {
    return begin_ <= o.begin_ && o.end_ <= end_;
  }
  // Touch at exactly one endpoint, sharing no bytes.
  bool Adjacent(const GuestRange& o) const {
    return end_ == o.begin_ || o.end_ == begin_;
  }
  bool operator==(const GuestRange& o) const {
    return begin_ == o.begin_ && end_ == o.end_;
  }
  bool operator!=(const GuestRange& o) const { return !(*this == o); }

  Relation Classify(const GuestRange& o) const {
    if (!Overlaps(o)) {
      return Adjacent(o) ? Relation::kAdjacent : Relation::kDisjoint;
    }
    if (*this == o) {
      return Relation::kExact;
    }
    if (Contains(o)) {
      return Relation::kContains;
    }
    if (o.Contains(*this)) {
      return Relation::kContainedBy;
    }
    return Relation::kPartial;
  }

  // The overlapping sub-interval, or nullopt if the ranges share no bytes.
  // (Adjacent ranges return nullopt: an empty intersection is not a range.)
  std::optional<GuestRange> Intersection(const GuestRange& o) const {
    uint32_t lo = std::max(begin_, o.begin_);
    uint32_t hi = std::min(end_, o.end_);
    if (lo >= hi) {
      return std::nullopt;
    }
    return GuestRange(lo, hi);
  }

 private:
  GuestRange(uint32_t begin, uint32_t end) : begin_(begin), end_(end) {}

  uint32_t begin_;
  uint32_t end_;
};

// An unordered collection of normalized guest ranges. This models a consumer's
// precise footprint (e.g. a texture's disjoint base-level and mip extents, or an
// aliased/multiple-consumer set) and a producer's checked destinations.
//
// Add() preserves each supplied range verbatim so genuinely disjoint extents are
// never fused into one conservative envelope. Normalize() coalesces only ranges
// that touch or overlap -- a lossless byte-coverage operation -- so base+mip
// extents separated by a gap stay two ranges while duplicate/aliased and
// contiguous ranges collapse. Overlap queries treat adjacency as non-overlap,
// matching GuestRange semantics.
class GuestRangeSet {
 public:
  GuestRangeSet() = default;

  // Append a normalized range. Skips nullopt so callers can forward factory
  // results directly. Returns true if a range was added.
  bool Add(const std::optional<GuestRange>& range) {
    if (!range) {
      return false;
    }
    ranges_.push_back(*range);
    return true;
  }
  void Add(const GuestRange& range) { ranges_.push_back(range); }

  // Drops the ranges and keeps the capacity (per-draw scratch reuse).
  void Clear() { ranges_.clear(); }
  bool empty() const { return ranges_.empty(); }
  size_t size() const { return ranges_.size(); }
  const std::vector<GuestRange>& ranges() const { return ranges_; }

  // Sum of bytes covered by the coalesced set (each byte counted once). Useful
  // for audit telemetry without exposing the internal layout.
  uint64_t CoveredBytes() const {
    GuestRangeSet normalized = *this;
    normalized.Normalize();
    uint64_t total = 0;
    for (const GuestRange& r : normalized.ranges_) {
      total += r.length();
    }
    return total;
  }

  // Coalesce touching/overlapping ranges into the minimal disjoint set, sorted
  // by begin. Ranges with a gap between them (base vs mip) are preserved.
  void Normalize() {
    if (ranges_.size() < 2) {
      return;
    }
    std::sort(ranges_.begin(), ranges_.end(),
              [](const GuestRange& a, const GuestRange& b) {
                if (a.start() != b.start()) {
                  return a.start() < b.start();
                }
                return a.end() < b.end();
              });
    // Merge in place: the write index never passes the read index, and no
    // vector is allocated per call (Normalize runs per draw).
    size_t out = 0;
    uint32_t cur_begin = ranges_.front().start();
    uint32_t cur_end = ranges_.front().end();
    for (size_t i = 1; i < ranges_.size(); ++i) {
      const GuestRange& r = ranges_[i];
      if (r.start() <= cur_end) {
        cur_end = std::max(cur_end, r.end());
      } else {
        ranges_[out++] = *GuestRange::FromBounds(cur_begin, cur_end);
        cur_begin = r.start();
        cur_end = r.end();
      }
    }
    ranges_[out++] = *GuestRange::FromBounds(cur_begin, cur_end);
    ranges_.erase(ranges_.begin() + ptrdiff_t(out), ranges_.end());
  }

  // True if any stored range shares a byte with the query. Adjacency is not
  // overlap, so a producer/consumer query returns false for merely touching
  // ranges (no drain required).
  bool OverlapsAny(const GuestRange& query) const {
    for (const GuestRange& r : ranges_) {
      if (r.Overlaps(query)) {
        return true;
      }
    }
    return false;
  }

  // True if any range in this set overlaps any range in other.
  bool OverlapsAny(const GuestRangeSet& other) const {
    for (const GuestRange& r : ranges_) {
      if (other.OverlapsAny(r)) {
        return true;
      }
    }
    return false;
  }

  // Collect the stored ranges (in stored order) that share a byte with the
  // query -- the selective-drain primitive: only intersecting producer records.
  std::vector<GuestRange> CollectOverlapping(const GuestRange& query) const {
    std::vector<GuestRange> out;
    for (const GuestRange& r : ranges_) {
      if (r.Overlaps(query)) {
        out.push_back(r);
      }
    }
    return out;
  }

 private:
  std::vector<GuestRange> ranges_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_GUEST_RANGE_H_
