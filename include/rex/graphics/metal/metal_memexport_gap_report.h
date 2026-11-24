/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_MEMEXPORT_GAP_REPORT_H_
#define XENIA_GPU_METAL_METAL_MEMEXPORT_GAP_REPORT_H_

// Bounded, unconditional reporting of memexport producer classes this backend
// does not route.
//
// WHY UNCONDITIONAL: the writes are simply absent from guest memory, so the
// symptom is missing or stale data with nothing to attribute it to. A report
// gated behind a switch cannot catch that, because nobody enables memexport
// logging until they already suspect memexport. No cvar or environment variable
// may be required to learn that Metal dropped an unsupported producer.
//
// WHY BOUNDED: an unbounded warning per draw or per range would flood the log
// of any title that engaged the condition, which is the case where the log
// matters most. Reports are deduplicated by producer class and normalized guest
// destination range, capped, and followed by exactly one suppression summary.
//
// GUEST STATE ONLY: the key and every emitted field are guest addresses, sizes,
// and producer classification. Host pointers never appear.
//
// This is production behavior, not a diagnostic probe. It is covered directly
// by conformance/metal_memexport_gap_report_test.cc.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace rex {
namespace graphics {
namespace metal {

// The producer classes this backend does not route. Kept distinct because they
// need different work to support and are separately measured: the 2026-07-28
// census returned NO-GO for all three, each for its own reason.
//
// UPDATE (extend-metal-rasterizing-memexport-producers): the two rasterizing
// classes are now ROUTABLE behind XE_METAL_MEMEXPORT_RASTER (default OFF).
// This reporter is unchanged and stays the authority on what is NOT routed:
// the caller observes a class only when the routing decision
// (metal_memexport_raster_flags.h) actually declined it, so the accounting
// flips per draw as classes route rather than when a gate flips. Pixel-stage
// export remains permanently unrouted and reported regardless of the gate.
enum class MemexportProducerClass : uint32_t {
  kRasterizingVertex = 0,          // VS export with a bound RT
  kRasterizingVertexGeometry,      // same, on a geometry-emulated primitive
  kPixelStage,                     // pixel-shader export
  kCount,
};

inline const char* MemexportProducerClassName(MemexportProducerClass c) {
  switch (c) {
    case MemexportProducerClass::kRasterizingVertex:
      return "rasterizing-vertex";
    case MemexportProducerClass::kRasterizingVertexGeometry:
      return "rasterizing-vertex-geometry-emulated";
    case MemexportProducerClass::kPixelStage:
      return "pixel-stage";
    default:
      return "unknown";
  }
}

// Decides whether an observed unrouted export should be reported, suppressed,
// or should emit the single suppression summary. Deliberately free of logging
// and of Metal types so the policy is directly testable.
class MemexportGapReporter {
 public:
  // Distinct (class, range) reports per title/session before suppression.
  // Small on purpose: the value of this report is discovering that the
  // condition exists at all, not enumerating every instance.
  static constexpr uint32_t kMaxDistinctReports = 16;

  enum class Action {
    kReport,                   // first sighting of this key, under the cap
    kSuppressed,               // duplicate key, or past the cap
    kEmitSuppressionSummary,   // exactly once, when the cap is first exceeded
  };

  Action Observe(MemexportProducerClass producer_class, uint32_t base_dwords,
                 uint32_t size_bytes) {
    const uint64_t key = MakeKey(producer_class, base_dwords, size_bytes);
    if (!seen_.insert(key).second) {
      ++suppressed_;
      return Action::kSuppressed;
    }
    if (distinct_reported_ < kMaxDistinctReports) {
      ++distinct_reported_;
      return Action::kReport;
    }
    ++suppressed_;
    if (!summary_emitted_) {
      summary_emitted_ = true;
      return Action::kEmitSuppressionSummary;
    }
    return Action::kSuppressed;
  }

  // Title/session boundary. Touches only this reporter's own state -- it does
  // not participate in cache clearing and holds no guest state.
  void Reset() {
    seen_.clear();
    distinct_reported_ = 0;
    suppressed_ = 0;
    summary_emitted_ = false;
  }

  uint32_t distinct_reported() const { return distinct_reported_; }
  uint64_t suppressed() const { return suppressed_; }
  bool summary_emitted() const { return summary_emitted_; }

 private:
  // Dedup key: producer class + normalized guest destination range. Two draws
  // exporting the same class to the same guest range are one finding; the same
  // range reached by a different class is not.
  static uint64_t MakeKey(MemexportProducerClass producer_class,
                          uint32_t base_dwords, uint32_t size_bytes) {
    return (uint64_t(uint32_t(producer_class)) << 56) ^
           (uint64_t(base_dwords) << 24) ^ uint64_t(size_bytes);
  }

  std::unordered_set<uint64_t> seen_;
  uint32_t distinct_reported_ = 0;
  uint64_t suppressed_ = 0;
  bool summary_emitted_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_MEMEXPORT_GAP_REPORT_H_
