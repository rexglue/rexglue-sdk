/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SERVING_CENSUS_H_
#define XENIA_GPU_METAL_METAL_SERVING_CENSUS_H_

#include <atomic>
#include <cstdint>

#include "rex/graphics/metal/metal_resolved_fetch_service.h"

namespace rex {
namespace graphics {
namespace metal {

// OBSERVATIONAL census for task 1.1 of
// implement-metal-gpu-visible-resolve-authority.
//
// The consumer-dependency declaration asks `MetalTextureCache::BridgeServes`
// whether a texture fetch needs its resolve range through RAM, while the serve
// chain decides how the same fetch is actually served with
// `ClassifyResolvedFetch`. Standing spec `metal-resolve-bridge-authority`
// ("One serve-order decision") forbids re-deriving the bridge/exact-cover
// conditions at a call site, and `BridgeServes` does exactly that — so the two
// can disagree. This counts the disagreement on real content BEFORE any
// behavior changes, so the repair is scoped by measurement rather than by the
// assumption that it must be worth something.
//
// A "spurious dependency" here means: the serve chain will serve this fetch
// GPU-side and never read RAM for it, yet the declaration still registered a
// resolve range, which can cost a full consumer boundary (commit + wait).
//
// Counters are process-lifetime and dumped PERIODICALLY: macXenos ignores
// SIGTERM and is killed, so no shutdown emission would ever run.
struct ServingCensus {
  static std::atomic<uint64_t>& Bucket(ResolvedFetchService service,
                                       bool bridge_served) {
    // [service][bridge_served]
    static std::atomic<uint64_t> s_buckets[5][2];
    return s_buckets[uint32_t(service)][bridge_served ? 1 : 0];
  }

  static void Record(ResolvedFetchService service, bool bridge_served) {
    Bucket(service, bridge_served).fetch_add(1, std::memory_order_relaxed);
  }

  static uint64_t Get(ResolvedFetchService service, bool bridge_served) {
    return Bucket(service, bridge_served).load(std::memory_order_relaxed);
  }

  // A fetch the serve chain serves GPU-side (never reads RAM).
  static bool IsGpuServed(ResolvedFetchService service) {
    return service == ResolvedFetchService::kBridgeSnapshot ||
           service == ResolvedFetchService::kExactCoverSnapshot ||
           service == ResolvedFetchService::kScaledCoverSnapshot;
  }

  // Fetches served GPU-side whose declaration still registered a resolve
  // dependency — the quantity this repair would remove.
  static uint64_t SpuriousDependencies() {
    return Get(ResolvedFetchService::kBridgeSnapshot, false) +
           Get(ResolvedFetchService::kExactCoverSnapshot, false);
  }

  // Declarations that skipped the resolve range for a fetch the serve chain
  // does NOT serve GPU-side. Analytically impossible (BridgeServes is strictly
  // narrower than the classifier), so a nonzero value falsifies that reading
  // and must be investigated before any behavior change.
  static uint64_t UnsafeSkips() {
    return Get(ResolvedFetchService::kNotResolveBacked, true) +
           Get(ResolvedFetchService::kRamContent, true);
  }

  // BOUNDARY-LEVEL census. Counting spurious DECLARATIONS overstates the
  // repair: a spurious range costs nothing unless it alone drives the
  // consumer decision. These count whole texture consumer requests, comparing
  // the decision the backend takes today against the decision it would take
  // with GPU-served fetches omitting their resolve range.
  struct Requests {
    static std::atomic<uint64_t>& Total() {
      static std::atomic<uint64_t> v;
      return v;
    }
    // Today: dependency (await or drain). Corrected: none. The repair turns
    // this request's boundary into no work at all -- the only cell that maps
    // to recovered frame time.
    static std::atomic<uint64_t>& BoundaryAvoided() {
      static std::atomic<uint64_t> v;
      return v;
    }
    // Dependency both ways: some other range needed it, so the repair changes
    // nothing for this request.
    static std::atomic<uint64_t>& BoundaryRetained() {
      static std::atomic<uint64_t> v;
      return v;
    }
    // Corrected has a dependency the current path does NOT: impossible if
    // BridgeServes is strictly narrower. Must stay zero.
    static std::atomic<uint64_t>& CorrectedStricter() {
      static std::atomic<uint64_t> v;
      return v;
    }
  };

  // TASK 2.1 — BOUNDARY ATTRIBUTION. Task 1.1 proved a boundary survives
  // unless a draw's WHOLE dependency set becomes GPU-satisfiable, so the
  // question Stage B turns on is: what actually holds each taken boundary
  // open? Bucketed per consumer kind by the producer states actually selected
  // (a boundary held only by memexport is untouched by a resolve-side
  // mechanism, and vice versa).
  enum class HeldBy : uint32_t {
    kResolveOnly = 0,
    kMemexportOnly = 1,
    kMixed = 2,
    kCount = 3,
  };

  static const char* HeldByName(HeldBy held) {
    switch (held) {
      case HeldBy::kResolveOnly:
        return "resolve_only";
      case HeldBy::kMemexportOnly:
        return "memexport_only";
      case HeldBy::kMixed:
        return "mixed";
      default:
        return "?";
    }
  }

  // [consumer kind][held-by]
  static std::atomic<uint64_t>& Boundary(uint32_t consumer_kind, HeldBy held) {
    static std::atomic<uint64_t> s_boundaries[8][uint32_t(HeldBy::kCount)];
    return s_boundaries[consumer_kind & 7][uint32_t(held)];
  }

  static void RecordBoundary(uint32_t consumer_kind, bool has_resolve,
                             bool has_memexport) {
    HeldBy held = has_resolve && has_memexport ? HeldBy::kMixed
                  : has_memexport              ? HeldBy::kMemexportOnly
                                               : HeldBy::kResolveOnly;
    Boundary(consumer_kind, held).fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SERVING_CENSUS_H_
