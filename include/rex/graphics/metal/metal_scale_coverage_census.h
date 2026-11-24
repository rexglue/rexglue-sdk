/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SCALE_COVERAGE_CENSUS_H_
#define XENIA_GPU_METAL_METAL_SCALE_COVERAGE_CENSUS_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <rex/logging/log_category.h>
#include "rex/graphics/metal/metal_resolved_fetch_service.h"

namespace rex {
namespace graphics {
namespace metal {

// OBSERVATIONAL census for task 0.1 of implement-metal-draw-resolution-scaling
// (XE_METAL_SCALE_CENSUS=1, default off). Counts only: it is never consulted
// by serving, declaration or any other decision.
//
// The question: under the proposed design, a texture fetch is rendered at the
// draw resolution scale only when it is served from a live resolve snapshot
// (kBridgeSnapshot / kExactCoverSnapshot). How much of the texture traffic that
// reads resolve-written memory is served that way, rather than from guest
// memory at 1x?
//
// Per metal-resolve-bridge-authority, resolve provenance is NOT inferred from
// snapshot registration. It is keyed directly: every resolve's guest
// destination byte range is recorded here at the copy, and a fetch is
// attributed to resolve output only when its base address lies inside a
// recorded range. The per-base lines name the fetch base and the resolve range
// that contains it, so the join can be audited offline.
class ScaleCoverageCensus {
 public:
  static ScaleCoverageCensus& Get() {
    static ScaleCoverageCensus census;
    return census;
  }

  // A resolve wrote guest bytes [start, start + length). Ranges are kept as
  // written (not merged with neighbours, which would sweep unrelated memory
  // such as memexport output into resolve provenance).
  void RecordResolve(uint32_t start, uint32_t length) {
    if (!length) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++window_resolves_;
    uint32_t& end = resolve_ranges_[start];
    end = std::max(end, start + length);
  }

  // One texture binding of one draw, with its serve chain classification.
  void RecordFetch(uint32_t base, uint32_t width, uint32_t height,
                   ResolvedFetchService service) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t area = uint64_t(width) * height;
    uint32_t range_start = 0;
    bool resolve_written = FindRange(base, range_start);
    Totals& totals = resolve_written ? resolve_written_ : other_;
    totals.count[uint32_t(service)] += 1;
    totals.area[uint32_t(service)] += area;
    if (resolve_written) {
      PerBase& per_base = per_base_[base];
      per_base.range_start = range_start;
      per_base.width = std::max(per_base.width, width);
      per_base.height = std::max(per_base.height, height);
      per_base.count[uint32_t(service)] += 1;
      per_base.area[uint32_t(service)] += area;
    }
  }

  // Emits the window's totals and the per-base attribution, then resets the
  // window. Resolve ranges persist across windows (provenance is cumulative).
  void Dump(uint32_t swap_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto line = [](const char* label, const Totals& t) {
      uint64_t count = 0, area = 0, snapshot_count = 0, snapshot_area = 0;
      for (uint32_t i = 0; i < 5; ++i) {
        count += t.count[i];
        area += t.area[i];
      }
      for (ResolvedFetchService s :
           {ResolvedFetchService::kBridgeSnapshot,
            ResolvedFetchService::kExactCoverSnapshot,
            ResolvedFetchService::kScaledCoverSnapshot}) {
        snapshot_count += t.count[uint32_t(s)];
        snapshot_area += t.area[uint32_t(s)];
      }
      return fmt::format(
          "{}: fetches={} snapshot={} ({:.1f}%) area_snapshot={:.1f}% "
          "[bridge={} exact_cover={} scaled_cover={} ram_content={} "
          "not_resolve_backed={}]",
          label, count, snapshot_count,
          count ? 100.0 * double(snapshot_count) / double(count) : 0.0,
          area ? 100.0 * double(snapshot_area) / double(area) : 0.0,
          t.count[uint32_t(ResolvedFetchService::kBridgeSnapshot)],
          t.count[uint32_t(ResolvedFetchService::kExactCoverSnapshot)],
          t.count[uint32_t(ResolvedFetchService::kScaledCoverSnapshot)],
          t.count[uint32_t(ResolvedFetchService::kRamContent)],
          t.count[uint32_t(ResolvedFetchService::kNotResolveBacked)]);
    };
    REXLOG_INFO("MetalScaleCensus swap#{}: resolves={} resolve_ranges={} {}; {}",
           swap_index, window_resolves_, resolve_ranges_.size(),
           line("resolve_written", resolve_written_), line("other", other_));
    for (const auto& kv : per_base_) {
      const PerBase& p = kv.second;
      auto cls = [&p](ResolvedFetchService s) {
        return fmt::format("{}/{}", p.count[uint32_t(s)], p.area[uint32_t(s)]);
      };
      REXLOG_INFO(
          "MetalScaleCensus base={:08X} in_resolve_range={:08X} exact_base={} "
          "max={}x{} count/area: bridge={} exact_cover={} scaled_cover={} "
          "ram_content={} not_resolve_backed={}",
          kv.first, p.range_start, kv.first == p.range_start ? 1 : 0, p.width,
          p.height, cls(ResolvedFetchService::kBridgeSnapshot),
          cls(ResolvedFetchService::kExactCoverSnapshot),
          cls(ResolvedFetchService::kScaledCoverSnapshot),
          cls(ResolvedFetchService::kRamContent),
          cls(ResolvedFetchService::kNotResolveBacked));
    }
    window_resolves_ = 0;
    resolve_written_ = {};
    other_ = {};
    per_base_.clear();
  }

 private:
  struct Totals {
    uint64_t count[5] = {};
    uint64_t area[5] = {};
  };
  struct PerBase {
    uint32_t range_start = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t count[5] = {};
    uint64_t area[5] = {};
  };

  // The containing range with the closest start. Ranges may overlap, so all
  // ranges starting at or below the address are considered.
  bool FindRange(uint32_t address, uint32_t& start_out) const {
    auto it = resolve_ranges_.upper_bound(address);
    while (it != resolve_ranges_.begin()) {
      --it;
      if (address < it->second) {
        start_out = it->first;
        return true;
      }
    }
    return false;
  }

  std::mutex mutex_;
  // Resolve destination ranges as written, start -> end (exclusive).
  std::map<uint32_t, uint32_t> resolve_ranges_;
  uint64_t window_resolves_ = 0;
  Totals resolve_written_;
  Totals other_;
  std::map<uint32_t, PerBase> per_base_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SCALE_COVERAGE_CENSUS_H_
