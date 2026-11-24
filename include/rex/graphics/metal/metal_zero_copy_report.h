/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ZERO_COPY_REPORT_H_
#define XENIA_GPU_METAL_METAL_ZERO_COPY_REPORT_H_

#include <atomic>
#include <cstdint>

#include "rex/graphics/metal/metal_zero_copy_contract.h"
#include "rex/graphics/metal/metal_zero_copy_overlap.h"

namespace rex {
namespace graphics {
namespace metal {

// Runtime cost vocabulary for the Metal zero-copy shared-memory pre-gate's
// group-6 live/trace evidence (tasks 6.3/6.4).
//
// The structural, device and mutation evidence is already accepted; what group
// 6 still has to answer is a COST question -- "does aliasing actually remove
// work, and at what price in ordering barriers and waits" -- and that question
// has to be answerable for the CONTROL ARMS too. Exact-off and forced-fallback
// runs install no observer and legitimately emit nothing through
// MetalZeroCopyOverlap, so if the cost numbers rode on that reporter the
// mirror-mode baseline the alias arm is compared against could never be
// measured. This vocabulary is therefore separate and backing-independent: the
// same line is emitted on every backing, and the alias/mirror verdict is one
// FIELD of it rather than its precondition.
//
// Three properties are load-bearing:
//
//   * EVENTS ARE COUNTED AT THE OPERATION THAT PRODUCES THEM. A commit is
//     counted at the commit, a wait that actually blocked at the block, a
//     memexport retirement at the retirement, bytes at the copy that moved
//     them. Nothing is derived from the backing mode, and nothing is derived
//     from a CONSULTATION: the ordering barrier counter advances only where a
//     predecessor drain really ran, not wherever the pending-producer snapshot
//     was read. A number inferred from configuration would restate the
//     configuration.
//
//   * COUNTERS SATURATE. These run for a whole session and are read as
//     evidence; a wrapped total would read as a small number, i.e. as "almost
//     no work happened", which is precisely the claim under test. Saturation
//     reuses ZeroCopySaturatingAdd from the overlap header rather than
//     restating it.
//
//   * DEFAULT OFF, AND OFF MEANS INERT. The meter refuses to advance until it
//     is enabled, so an unset run is silent BY STRUCTURE rather than by a
//     caller remembering not to print. Nothing here allocates, waits, logs, or
//     calls back into the backend, so the sites that count are free to sit on
//     the GPU thread's hot path; none of them sits on the faulting guest
//     thread, which may not log at all (see metal_zero_copy_overlap.h).
//
// This reporter is EVIDENCE, never a gate: no promotion decision reads it, and
// nothing in the rendering or authority path branches on it.

// The exact-off diagnostic gate for the runtime cost line.
inline constexpr const char* kZeroCopyReportEnvVar = "XE_METAL_ZERO_COPY_REPORT";

// Exact opt-in, deliberately the SAME pure rule the alias gate parses with
// (metal_zero_copy_contract.h) rather than a second, subtly different one: two
// spellings of "is this enabled" is how a control arm ends up silently running
// with diagnostics the other arm does not have. nullptr (unset), "", "0",
// "01", " 1", "1\n" and "true" are all off.
constexpr bool IsZeroCopyReportEnabled(const char* value) {
  return IsZeroCopyGateEnabled(value);
}

// One counter per PRODUCTION OPERATION the cost question is asked about.
//
// Upload bytes (copied vs alias-skipped) and every overlap-observer number are
// deliberately NOT here: they are already recorded at their own operations by
// MetalSharedMemory and ZeroCopyOverlapObserver, and the reporter reads those
// existing snapshots. Duplicating them would create a second source that could
// disagree with the first.
enum class ZeroCopyCostCounter : uint32_t {
  // Frame/submission command-buffer commits. Auxiliary one-off command buffers
  // (readbacks, blits, present conversion) are NOT counted here -- they mint no
  // submission and are not part of the submission cadence this measures.
  kSubmissionCommits,
  // Commits that were followed by an explicit synchronous wait on that same
  // command buffer (the default XE_METAL_SYNC_WAIT path). Separate from the
  // commit count because a commit that returns immediately and a commit that
  // stalls the GPU thread are different costs wearing one name.
  kSyncCommitWaits,
  // Every WaitForSubmission call, including the ones satisfied by the
  // already-completed fast path...
  kWaitForSubmissionCalls,
  // ...and, separately, the ones that actually reached the condition variable.
  // Reporting only the call count would make a cheap already-done check look
  // like a stall; reporting only the blocked count would hide how often the
  // path is taken at all.
  //
  // "Reached the condition variable" is a PROVEN fact at the production site,
  // not an approximation of one: the waiter re-checks completion while holding
  // the same mutex the completion publisher advances the high-water under, and
  // holds it through the wait's first predicate evaluation. Without that shared
  // boundary the publisher could overtake the re-check and the wait would be
  // counted as blocked while never waiting -- an ATTEMPTED wait reported as a
  // stall, which the census rules forbid. What the counter does not claim is
  // duration: a wait entered and satisfied microseconds later is one block.
  kWaitForSubmissionBlocked,
  // Waits that expired instead of being satisfied. Counted apart from blocked
  // waits because a timeout is a wedged GPU, not a slow one, and a run
  // containing any is not a run whose cost numbers describe healthy work.
  kWaitForSubmissionTimeouts,
  // Memexport destination preparations attempted (one per producer draw that
  // reached the ordered-preparation seam). The fast-path and barrier counters
  // below are disjoint subsets of this, and their sum is a LOWER bound rather
  // than an identity: a draw that produced no normalizable destination range
  // never reaches an ordering verdict at all, so it is counted here and among
  // the failures and in neither of the two.
  kMemexportDestinationPreparations,
  // Preparations whose pending-producer snapshot selected nothing, so the store
  // was already last and the preparation cost no barrier at all. This is the
  // path aliasing is expected to leave alone; separating it is what keeps the
  // barrier count below from absorbing the cheap case.
  kMemexportNoOlderFastPaths,
  // Preparations where an ordering barrier REALLY RAN -- older overlapping
  // producers were selected and the drain (commit + wait + convergence) was
  // executed. Counted where the drain is invoked, never where the snapshot is
  // consulted.
  kMemexportOrderingBarriers,
  // Older overlapping producer records selected by those barriers. The barrier
  // count says how often the backend stopped; this says how much it had to
  // retire.
  kMemexportSelectedPredecessors,
  // Barriers that ended with at least one predecessor STILL PENDING. A short
  // drain retains the destination rather than being rounded up to success, so
  // this is a correctness-relevant outcome and not a cost footnote.
  kMemexportOrderingRetained,
  // Preparations that did not end prepared, for any reason (unordered, or a
  // destination range request that failed). Includes the retained outcomes
  // above, which are also counted on their own so the two causes stay
  // distinguishable.
  kMemexportPreparationFailures,
  // Pending memexport records actually retired by convergence. Counted at the
  // retirement itself and INDEPENDENTLY of the byte counter below: an aliased
  // backing retires the same records having moved zero bytes, and a record that
  // was retained (short drain, unproven finality, failed copy) never reaches
  // the retirement site at all.
  kMemexportRetirements,
  // Bytes memexport convergence PHYSICALLY copied, summed from the shared
  // memory's own factual outcome. Zero on the proven-alias path.
  kMemexportConvergenceBytesCopied,
  // Not a value: one past the last real enumerator. Every enumerator must be
  // declared ABOVE this line so the count below, and the conformance arrays
  // pinned to it, follow the enum automatically.
  kCount,
};
inline constexpr uint32_t kZeroCopyCostCounterCount =
    static_cast<uint32_t>(ZeroCopyCostCounter::kCount);

// kCount is a bound, never a counter. Anything at or past it indexes nothing.
constexpr bool IsZeroCopyCostCounterValue(ZeroCopyCostCounter value) {
  return static_cast<uint32_t>(value) < kZeroCopyCostCounterCount;
}

// Exhaustive switch, no default: adding an enumerator without naming it is a
// compiler warning here rather than an unnamed number in the evidence line.
constexpr const char* ZeroCopyCostCounterName(ZeroCopyCostCounter value) {
  switch (value) {
    case ZeroCopyCostCounter::kSubmissionCommits:
      return "submission_commits";
    case ZeroCopyCostCounter::kSyncCommitWaits:
      return "sync_commit_waits";
    case ZeroCopyCostCounter::kWaitForSubmissionCalls:
      return "wait_calls";
    case ZeroCopyCostCounter::kWaitForSubmissionBlocked:
      return "wait_blocked";
    case ZeroCopyCostCounter::kWaitForSubmissionTimeouts:
      return "wait_timeouts";
    case ZeroCopyCostCounter::kMemexportDestinationPreparations:
      return "memexport_preparations";
    case ZeroCopyCostCounter::kMemexportNoOlderFastPaths:
      return "memexport_no_older";
    case ZeroCopyCostCounter::kMemexportOrderingBarriers:
      return "memexport_barriers";
    case ZeroCopyCostCounter::kMemexportSelectedPredecessors:
      return "memexport_predecessors";
    case ZeroCopyCostCounter::kMemexportOrderingRetained:
      return "memexport_retained";
    case ZeroCopyCostCounter::kMemexportPreparationFailures:
      return "memexport_prep_failed";
    case ZeroCopyCostCounter::kMemexportRetirements:
      return "memexport_retirements";
    case ZeroCopyCostCounter::kMemexportConvergenceBytesCopied:
      return "memexport_bytes_copied";
    case ZeroCopyCostCounter::kCount:
      break;  // not a value; falls to the out-of-range bucket
  }
  return "unknown";
}

// One coherent read of the meter. Fixed size, no allocation: the reporter
// formats from this rather than loading counters one at a time while the
// session keeps running.
struct ZeroCopyCostSnapshot {
  // Whether the meter was enabled when the snapshot was taken. Kept alongside
  // the values so a disabled run's snapshot cannot be mistaken for an enabled
  // run that happened to do nothing.
  bool enabled = false;
  uint64_t values[kZeroCopyCostCounterCount] = {};

  // Out-of-range reads return zero rather than indexing past the array. A
  // report that asked for an undefined counter must not be able to print
  // whatever byte followed it.
  uint64_t Get(ZeroCopyCostCounter counter) const {
    return IsZeroCopyCostCounterValue(counter)
               ? values[static_cast<uint32_t>(counter)]
               : 0;
  }
};

// The silence rule for the default (unset/0/malformed) arm: nothing enabled and
// nothing advanced. Both halves are required, so a meter that somehow counted
// while disabled is NOT silent and the caller must say so rather than print
// nothing and call it a clean run.
constexpr bool ZeroCopyCostSnapshotIsSilent(const ZeroCopyCostSnapshot& in) {
  if (in.enabled) {
    return false;
  }
  for (uint32_t i = 0; i < kZeroCopyCostCounterCount; ++i) {
    if (in.values[i]) {
      return false;
    }
  }
  return true;
}

// The bounded meter. Fixed-size, allocation-free, lock-free, and inert until
// enabled.
//
// Threading: counted from the GPU thread's production sites and read at a GPU
// thread lifecycle boundary. The counters are atomic anyway, because
// WaitForSubmission is reachable from any thread that drives a submission
// boundary, and an evidence counter must not be the thing that introduces a
// data race. Enable/Reset are NOT concurrent with counting: the owner enables
// once during context setup, before any frame exists.
class ZeroCopyCostMeter {
 public:
  ZeroCopyCostMeter() = default;
  ZeroCopyCostMeter(const ZeroCopyCostMeter&) = delete;
  ZeroCopyCostMeter& operator=(const ZeroCopyCostMeter&) = delete;

  void SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
  }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // Adding nothing, adding to an undefined counter, and adding while disabled
  // are all no-ops -- the disabled case first, so an off run cannot be told
  // apart from a build without the meter by looking at the counters.
  void Add(ZeroCopyCostCounter counter, uint64_t added) {
    if (!enabled() || !IsZeroCopyCostCounterValue(counter) || !added) {
      return;
    }
    ZeroCopySaturatingAdd(counters_[static_cast<uint32_t>(counter)], added);
  }
  void Count(ZeroCopyCostCounter counter) { Add(counter, 1); }

  ZeroCopyCostSnapshot Snapshot() const {
    ZeroCopyCostSnapshot out;
    out.enabled = enabled();
    for (uint32_t i = 0; i < kZeroCopyCostCounterCount; ++i) {
      out.values[i] = counters_[i].load(std::memory_order_relaxed);
    }
    return out;
  }

  // Clears the counters and leaves the enable state alone: a session boundary
  // resets what was measured, not whether measuring was requested.
  void ResetCounters() {
    for (uint32_t i = 0; i < kZeroCopyCostCounterCount; ++i) {
      counters_[i].store(0, std::memory_order_relaxed);
    }
  }

 private:
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> counters_[kZeroCopyCostCounterCount] = {};
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ZERO_COPY_REPORT_H_
