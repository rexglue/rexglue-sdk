/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SEAM_TIMING_H_
#define XENIA_GPU_METAL_METAL_SEAM_TIMING_H_

#include <cstdint>
#include <cstdio>

#include "rex/graphics/metal/metal_memory_authority.h"

namespace rex {
namespace graphics {
namespace metal {

// Submission-boundary attribution (optimize-native-metal-frametime task 3.1,
// contract 2026-08-08). Two layers:
//
//   GLOBAL:   every EndFrameEncoder / EndFrameEncoderAndWait call carries an
//             explicit SeamBoundaryReason -- no default parameter exists, so a
//             new call site fails to compile until it declares why it is
//             closing the frame command buffer. This layer answers "where do
//             all 27 (VP) / 38 (JC2) commits/frame come from".
//   CONSUMER: ResolvePendingProducerDependencies additionally attributes by
//             its decision's ConsumerKind. This layer decomposes the
//             memexport/consumer-request corridor.
//
// TIMING HONESTY: on the DEFAULT path ([command_buffer waitUntilCompleted]
// inside EndFrameEncoder, XE_METAL_ASYNC unset) the principal GPU stall
// happens INSIDE the encoder-close function, and the explicit
// WaitForSubmission in EndFrameEncoderAndWait is normally pre-satisfied.
// Stages are therefore recorded separately: `close` is CPU-side encoder end +
// commit EXCLUDING the wait; `gpu_wait` is the actual waitUntilCompleted (or,
// under XE_METAL_ASYNC, the pipeline-depth-cap WaitForSubmission taken inside
// EndFrameEncoder -- same slot, disclosed by the run's env); `post_wait` is
// the AndWait wrapper's conditional WaitForSubmission. `inclusive` (consumer
// layer) covers the whole request and MUST NOT be summed with its nested
// stages -- it already contains them.
//
// The accumulator is pure arithmetic over caller-supplied nanosecond values:
// no clock, no allocation, no logging. Production reads a monotonic clock and
// feeds deltas; tests feed synthetic values (fake clock) and assert the
// bookkeeping, never real durations.

enum class SeamBoundaryReason : uint32_t {
  kConsumerDependency,  // ResolvePendingProducerDependencies boundary
  kSwapPacing,          // the swap's one synchronous point (S8)
  kResolveSizeSplit,    // >= 1 MiB resolve commit split
  kArenaSplit,          // open submission holds a quarter of the upload arena
  kHazardFlush,         // shared-memory hazard flush callback
  kQueryAdmission,      // ZPD admission kDeferAfterClosingSubmission
  kQueryAwait,          // AwaitQueryResolve open-segment commit
  kCacheClear,          // ExecuteDeferredCacheClear safety boundary
  kReplayReset,         // ResetDerivedReplayState quiesce
  kEdramDump,           // DumpEdramToBuffer content finalization
  kDebugProbe,          // env-gated dump/probe/serialize diagnostics
  kShutdown,            // ShutdownContext final drain
  kCount,
};

inline const char* SeamBoundaryReasonName(SeamBoundaryReason reason) {
  switch (reason) {
    case SeamBoundaryReason::kConsumerDependency:
      return "consumer-dependency";
    case SeamBoundaryReason::kSwapPacing:
      return "swap-pacing";
    case SeamBoundaryReason::kResolveSizeSplit:
      return "resolve-size-split";
    case SeamBoundaryReason::kArenaSplit:
      return "arena-split";
    case SeamBoundaryReason::kHazardFlush:
      return "hazard-flush";
    case SeamBoundaryReason::kQueryAdmission:
      return "query-admission";
    case SeamBoundaryReason::kQueryAwait:
      return "query-await";
    case SeamBoundaryReason::kCacheClear:
      return "cache-clear";
    case SeamBoundaryReason::kReplayReset:
      return "replay-reset";
    case SeamBoundaryReason::kEdramDump:
      return "edram-dump";
    case SeamBoundaryReason::kDebugProbe:
      return "debug-probe";
    case SeamBoundaryReason::kShutdown:
      return "shutdown";
    case SeamBoundaryReason::kCount:
      break;
  }
  return nullptr;
}

// DIRECT waits (frametime task 3.1 completion): WaitForSubmission call sites
// that are neither submission boundaries nor consumer requests. Under the
// promoted async default these are live stall candidates; before this layer
// they sat in the census RESIDUAL. No default -- a new direct-wait site must
// name itself.
enum class SeamDirectWait : uint32_t {
  kArenaReclaim,   // upload-ring full: wait for oldest in-flight, reclaim
  kZpdAdmission,   // ZPD admission kWaitForOldestResolve
  kQueryResolve,   // AwaitQueryResolve completion wait
  kCount,
};

inline const char* SeamDirectWaitName(SeamDirectWait w) {
  switch (w) {
    case SeamDirectWait::kArenaReclaim:
      return "arena-reclaim";
    case SeamDirectWait::kZpdAdmission:
      return "zpd-admission";
    case SeamDirectWait::kQueryResolve:
      return "query-resolve";
    case SeamDirectWait::kCount:
      break;
  }
  return nullptr;
}

// Which term of required_submission = max(boundary, max_wait) dominated.
enum class SeamBindingTerm : uint32_t {
  kBoundary,  // the request's own just-committed boundary
  kProducer,  // an older selected producer's submission
  kTie,       // equal AND nonzero
  kZero,      // both terms zero: untagged producers with no open buffer --
              // representable (e.g. right after a prior boundary) and NOT a
              // tie in any meaningful sense, so it is not counted as one
  kCount,
};

struct SeamStageTotals {
  uint64_t count = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  void Record(uint64_t ns) {
    ++count;
    total_ns += ns;
    if (ns > max_ns) max_ns = ns;
  }
  void Reset() { count = total_ns = max_ns = 0; }
};

// Pinned to the enum's own sentinel: a kind added to ConsumerKind grows this
// automatically, and the device-free test rejects any index below kCount
// whose name is missing or the "?" fallback -- an unnamed kind cannot land.
inline constexpr uint32_t kSeamConsumerKindCount =
    uint32_t(ConsumerKind::kCount);

struct SeamTimingAccumulator {
  static constexpr uint32_t kReasonCount =
      uint32_t(SeamBoundaryReason::kCount);
  static constexpr uint32_t kConsumerCount = kSeamConsumerKindCount;

  // GLOBAL layer, indexed by SeamBoundaryReason.
  struct PerReason {
    uint64_t boundaries = 0;  // calls that closed a live command buffer
    uint64_t noop_calls = 0;  // no buffer open: free, must not be conflated
    SeamStageTotals close;     // function entry (incl. EndRenderEncoder)
                               // through [command_buffer commit] -- exactly
                               // "encoder close through commit", no more
    SeamStageTotals gpu_wait;  // the actual stall (see header comment)
    SeamStageTotals retire;    // post-wait completion-action bookkeeping +
                               // PumpCompletedActions -- named for what it
                               // is, NOT folded into close
    SeamStageTotals post_wait; // AndWait wrapper's conditional wait
    uint64_t post_wait_blocked = 0;  // WaitForSubmission's LOCKED re-check
                                     // proved the CV wait was entered (its
                                     // out_did_block); never the unlocked
                                     // pre-check, which races
    void Reset() {
      boundaries = noop_calls = post_wait_blocked = 0;
      close.Reset();
      gpu_wait.Reset();
      retire.Reset();
      post_wait.Reset();
    }
  };
  PerReason reason[kReasonCount];

  // CONSUMER layer, indexed by ConsumerKind.
  struct PerConsumer {
    uint64_t consultations = 0;      // Resolve... entered
    uint64_t empty_early_outs = 0;   // selected.empty(): no commit/wait/drain
    uint64_t non_empty = 0;
    uint64_t cb_present_commits = 0;  // live buffer at entry -> one commit
    uint64_t selected_resolve = 0;
    uint64_t selected_memexport = 0;
    uint64_t drained_records = 0;
    uint64_t copied_bytes = 0;
    uint64_t invalidated_entries = 0;
    uint64_t completed = 0;
    uint64_t failed = 0;
    uint64_t binding[uint32_t(SeamBindingTerm::kCount)] = {};
    uint64_t wait_requested = 0;  // required > completed at the pre-check
    uint64_t wait_blocked = 0;    // WaitForSubmission's LOCKED re-check proved
                                  // the CV wait was entered (out_did_block);
                                  // a fast-path race counts as requested only
    uint64_t wait_timeout = 0;    // WaitForSubmission returned false
    SeamStageTotals required_wait;  // conditional WaitForSubmission only
    SeamStageTotals drain;          // DrainSelectedPendingProducers
    SeamStageTotals inclusive;      // whole request; CONTAINS the above
    void Reset() {
      consultations = empty_early_outs = non_empty = cb_present_commits = 0;
      selected_resolve = selected_memexport = drained_records = 0;
      copied_bytes = invalidated_entries = completed = failed = 0;
      for (auto& b : binding) b = 0;
      wait_requested = wait_blocked = wait_timeout = 0;
      required_wait.Reset();
      drain.Reset();
      inclusive.Reset();
    }
  };
  PerConsumer consumer[kConsumerCount];
  uint64_t invalid_consumers = 0;

  // DIRECT layer, indexed by SeamDirectWait (task 3.1 completion): direct
  // WaitForSubmission sites outside boundaries and consumer requests.
  struct PerDirect {
    uint64_t calls = 0;      // times the site reached its wait
    uint64_t blocked = 0;    // LOCKED re-check proved the CV wait was entered
    uint64_t timeouts = 0;   // WaitForSubmission returned false
    SeamStageTotals wait;    // wall time of the WaitForSubmission call
    void Reset() {
      calls = blocked = timeouts = 0;
      wait.Reset();
    }
  };
  PerDirect direct[uint32_t(SeamDirectWait::kCount)];

  void Reset() {
    for (auto& r : reason) r.Reset();
    for (auto& c : consumer) c.Reset();
    for (auto& d : direct) d.Reset();
    invalid_consumers = 0;
  }

  // Checked consumer indexing: an out-of-range kind (including the kCount
  // sentinel) is explicit invalid evidence, never evidence for a real
  // corridor. The nullable result also prevents a caller from writing through
  // an invalid index after the observation has been counted.
  PerConsumer* ConsumerSlot(ConsumerKind kind) {
    uint32_t i = uint32_t(kind);
    if (i >= kConsumerCount) {
      ++invalid_consumers;
      return nullptr;
    }
    return &consumer[i];
  }

  bool AnyActivity() const {
    for (const auto& r : reason) {
      if (r.boundaries || r.noop_calls) return true;
    }
    for (const auto& c : consumer) {
      if (c.consultations) return true;
    }
    for (const auto& d : direct) {
      if (d.calls) return true;
    }
    return invalid_consumers != 0;
  }

  // Formats the non-zero entries into `buf`. Two lines' worth of content in
  // one call each; snprintf into caller storage, no allocation. Durations in
  // microseconds (integer) -- the decomposition question is ms-scale.
  // Returns chars written (clamped).
  size_t FormatReasons(char* buf, size_t cap) const {
    if (!cap) return 0;
    // A zero-activity side still yields a valid empty C string: both buffers
    // are always passed to the logger, so an untouched buffer must never be
    // uninitialized stack memory.
    buf[0] = '\0';
    size_t n = 0;
    auto put = [&](const char* fmt, auto... args) {
      if (n < cap) {
        int w = std::snprintf(buf + n, cap - n, fmt, args...);
        if (w > 0) n += size_t(w) < cap - n ? size_t(w) : cap - n;
      }
    };
    for (uint32_t i = 0; i < kReasonCount; ++i) {
      const PerReason& r = reason[i];
      if (!r.boundaries && !r.noop_calls) continue;
      put(" %s=%llu(noop %llu, close %lluus/max%llu, gpuwait %lluus/max%llu,"
          " retire %lluus/max%llu, postwait %lluus/max%llu blk%llu)",
          SeamBoundaryReasonName(SeamBoundaryReason(i)),
          (unsigned long long)r.boundaries, (unsigned long long)r.noop_calls,
          (unsigned long long)(r.close.total_ns / 1000),
          (unsigned long long)(r.close.max_ns / 1000),
          (unsigned long long)(r.gpu_wait.total_ns / 1000),
          (unsigned long long)(r.gpu_wait.max_ns / 1000),
          (unsigned long long)(r.retire.total_ns / 1000),
          (unsigned long long)(r.retire.max_ns / 1000),
          (unsigned long long)(r.post_wait.total_ns / 1000),
          (unsigned long long)(r.post_wait.max_ns / 1000),
          (unsigned long long)r.post_wait_blocked);
    }
    return n;
  }

  size_t FormatDirect(char* buf, size_t cap) const {
    if (!cap) return 0;
    buf[0] = '\0';  // see FormatReasons: never an uninitialized C string
    size_t n = 0;
    auto put = [&](const char* fmt, auto... args) {
      if (n < cap) {
        int w = std::snprintf(buf + n, cap - n, fmt, args...);
        if (w > 0) n += size_t(w) < cap - n ? size_t(w) : cap - n;
      }
    };
    for (uint32_t i = 0; i < uint32_t(SeamDirectWait::kCount); ++i) {
      const PerDirect& d = direct[i];
      if (!d.calls) continue;
      put(" %s=%llu(blk %llu, to %llu, wait %lluus/max%llu)",
          SeamDirectWaitName(SeamDirectWait(i)), (unsigned long long)d.calls,
          (unsigned long long)d.blocked, (unsigned long long)d.timeouts,
          (unsigned long long)(d.wait.total_ns / 1000),
          (unsigned long long)(d.wait.max_ns / 1000));
    }
    return n;
  }

  size_t FormatConsumers(char* buf, size_t cap) const {
    if (!cap) return 0;
    buf[0] = '\0';  // see FormatReasons: never an uninitialized C string
    size_t n = 0;
    auto put = [&](const char* fmt, auto... args) {
      if (n < cap) {
        int w = std::snprintf(buf + n, cap - n, fmt, args...);
        if (w > 0) n += size_t(w) < cap - n ? size_t(w) : cap - n;
      }
    };
    if (invalid_consumers) {
      put(" invalid-consumer=%llu", (unsigned long long)invalid_consumers);
    }
    for (uint32_t i = 0; i < kConsumerCount; ++i) {
      const PerConsumer& c = consumer[i];
      if (!c.consultations) continue;
      put(" %s=%llu(empty %llu, ne %llu, commits %llu, sel r%llu/m%llu,"
          " drained %llu, bytes %llu, inval %llu, ok %llu fail %llu,"
          " bind b%llu/p%llu/t%llu/z%llu, rw req%llu blk%llu to%llu"
          " %lluus/max%llu,"
          " drain %lluus/max%llu, incl %lluus/max%llu)",
          ConsumerKindName(ConsumerKind(i)),
          (unsigned long long)c.consultations,
          (unsigned long long)c.empty_early_outs,
          (unsigned long long)c.non_empty,
          (unsigned long long)c.cb_present_commits,
          (unsigned long long)c.selected_resolve,
          (unsigned long long)c.selected_memexport,
          (unsigned long long)c.drained_records,
          (unsigned long long)c.copied_bytes,
          (unsigned long long)c.invalidated_entries,
          (unsigned long long)c.completed, (unsigned long long)c.failed,
          (unsigned long long)c.binding[0], (unsigned long long)c.binding[1],
          (unsigned long long)c.binding[2], (unsigned long long)c.binding[3],
          (unsigned long long)c.wait_requested,
          (unsigned long long)c.wait_blocked,
          (unsigned long long)c.wait_timeout,
          (unsigned long long)(c.required_wait.total_ns / 1000),
          (unsigned long long)(c.required_wait.max_ns / 1000),
          (unsigned long long)(c.drain.total_ns / 1000),
          (unsigned long long)(c.drain.max_ns / 1000),
          (unsigned long long)(c.inclusive.total_ns / 1000),
          (unsigned long long)(c.inclusive.max_ns / 1000));
    }
    return n;
  }
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SEAM_TIMING_H_
