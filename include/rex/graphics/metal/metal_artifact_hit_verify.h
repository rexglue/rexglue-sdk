/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ARTIFACT_HIT_VERIFY_H_
#define XENIA_GPU_METAL_METAL_ARTIFACT_HIT_VERIFY_H_

// The cache-hit correctness invariant for
// adopt-xenios-shader-artifact-caching (task 2.1 item 4, consumed by 2.3):
//
//   A HIT MUST EQUAL A FRESH PRODUCTION OF THE SAME ARTIFACT BYTE FOR BYTE,
//   OR IT MUST MISS.
//
// WHY THIS IS THE LOAD-BEARING RULE, AND NOT A NICETY. The change this file
// belongs to widens caching from one artifact kind to several. Caching is
// precisely where this project's worst shader bug did its damage: the dxilconv
// converter-reuse defect produced CORRUPT quad-domain DXIL, and content-keyed
// caches then preserved and re-served that corruption because no key could see
// the wrapper behavior that caused it (metal_pipeline_storage.h,
// kDxilconvGeneration; the clip relabel, kFetchAbiRevision, is the same shape).
// Every artifact kind added is another surface on which a future correctness
// bug is not merely committed but PERSISTED. Key discipline is the primary
// defense and it is fallible by construction — a key cannot encode a behavior
// nobody knew had changed. This predicate is the second, behavioral defense:
// it does not ask whether the key looks right, it asks whether the bytes ARE
// what this build produces right now.
//
// Deliberately dumb: no tolerance, no normalization, no "close enough". A
// verdict other than kServe is a MISS, and a miss costs one cold production —
// the cheapest possible wrong answer. Anything smarter here would be a place
// for corruption to be argued into acceptance.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rex {
namespace graphics {
namespace metal {

enum class MetalArtifactHitVerdict {
  // The cached bytes are exactly what a fresh production yields. Serve it.
  kServe,
  // Nothing usable was cached — an absent, zero-length, or truncated-to-empty
  // entry. Distinguished from a mismatch because it means "cold", not
  // "something is wrong".
  kMissEmpty,
  // A fresh production yielded nothing, so there is nothing to compare
  // against. The hit is NOT servable: an unverifiable hit is treated exactly
  // like a failed one, because "we could not check" must never read as "it
  // checked out".
  kMissUnverifiable,
  // Cached and fresh differ in length.
  kMissSizeMismatch,
  // Same length, different bytes. The dxilconv-corruption shape.
  kMissByteMismatch,
};

inline const char* MetalArtifactHitVerdictName(MetalArtifactHitVerdict v) {
  switch (v) {
    case MetalArtifactHitVerdict::kServe:
      return "serve";
    case MetalArtifactHitVerdict::kMissEmpty:
      return "miss:empty";
    case MetalArtifactHitVerdict::kMissUnverifiable:
      return "miss:unverifiable";
    case MetalArtifactHitVerdict::kMissSizeMismatch:
      return "miss:size";
    case MetalArtifactHitVerdict::kMissByteMismatch:
      return "miss:bytes";
  }
  return "miss:unknown";
}

// Exactly one verdict means serve. Written as an equality rather than a
// "not a miss" test so a verdict added later defaults to NOT serving.
inline bool MetalArtifactHitIsServable(MetalArtifactHitVerdict v) {
  return v == MetalArtifactHitVerdict::kServe;
}

inline MetalArtifactHitVerdict VerifyMetalArtifactHit(const void* cached_data,
                                                      size_t cached_size,
                                                      const void* fresh_data,
                                                      size_t fresh_size) {
  if (!cached_data || cached_size == 0) {
    return MetalArtifactHitVerdict::kMissEmpty;
  }
  if (!fresh_data || fresh_size == 0) {
    return MetalArtifactHitVerdict::kMissUnverifiable;
  }
  if (cached_size != fresh_size) {
    return MetalArtifactHitVerdict::kMissSizeMismatch;
  }
  return std::memcmp(cached_data, fresh_data, cached_size) == 0
             ? MetalArtifactHitVerdict::kServe
             : MetalArtifactHitVerdict::kMissByteMismatch;
}

// Container-friendly overload for the byte vectors the storage layer deals in.
template <typename CachedContainer, typename FreshContainer>
inline MetalArtifactHitVerdict VerifyMetalArtifactHit(
    const CachedContainer& cached, const FreshContainer& fresh) {
  return VerifyMetalArtifactHit(cached.empty() ? nullptr : cached.data(),
                                cached.size(),
                                fresh.empty() ? nullptr : fresh.data(),
                                fresh.size());
}

// Verification telemetry. `mismatches` is the number that matters: it is
// supposed to be 0 forever, and a nonzero value is a correctness incident, not
// a performance note — it means a key admitted an artifact this build does not
// produce. Atomic because stage 5 (async pipeline compilation) makes the
// compile path multi-threaded; the counters must not become the reason that
// stage needs a lock.
struct MetalArtifactHitStats {
  std::atomic<uint64_t> verified{0};   // hits that were byte-compared
  std::atomic<uint64_t> served{0};     // of those, verdict == kServe
  std::atomic<uint64_t> mismatches{0}; // of those, anything else

  void Record(MetalArtifactHitVerdict verdict) {
    verified.fetch_add(1, std::memory_order_relaxed);
    if (MetalArtifactHitIsServable(verdict)) {
      served.fetch_add(1, std::memory_order_relaxed);
    } else {
      mismatches.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ARTIFACT_HIT_VERIFY_H_
