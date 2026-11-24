/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ZERO_COPY_OVERLAP_H_
#define XENIA_GPU_METAL_METAL_ZERO_COPY_OVERLAP_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace rex {
namespace graphics {
namespace metal {

// In-flight CPU rewrite evidence for the Metal zero-copy shared-memory pre-gate
// (pre-gate-metal-zero-copy-shared-memory tasks 5.1-5.4, design D6).
//
// Mirroring guest RAM into a separate MTLBuffer gives the backend a snapshot: a
// CPU write lands in guest RAM and only reaches the GPU at the next upload,
// which the hazard-flush path can order against already-encoded readers.
// Aliasing removes that snapshot. A guest CPU write now changes, in place, the
// exact bytes a queued-but-unfinished GPU read is going to consume. XeniOS
// recorded that risk and did not measure it; this pre-gate measures it.
//
// The measurement rests on TWO INDEPENDENT SOURCES, because a single source
// would be recording its own opinion:
//
//   * EXECUTION  -- which submission most recently enrolled a host page as a
//                   shared-memory GPU consumer. Recorded on the GPU thread at
//                   MetalSharedMemory::RequestConsumedRange, from a provider
//                   that reports the submission currently being ENCODED.
//   * OBSERVATION -- that the guest CPU physically wrote a page, seen on the
//                   faulting guest thread at the one physical-memory
//                   invalidation entry, compared against a COMPLETED
//                   high-water mark supplied separately by the command
//                   processor.
//
// Neither source can produce an overlap alone: without a stamp every page reads
// idle, and without a callback nothing is ever compared. That is the point --
// "no overlap was reported" has to be falsifiable, and a run whose enrollment
// source is broken must not look like a clean run. `zero_submission_stamp_pages`
// exists for exactly that: a stamp of zero is the idle sentinel, so a provider
// that returns zero (the S7 tagless consumed-range provider does, on every
// default launch) would silently make every page unobservable. Counting those
// stamps means the report says so instead.
//
// Everything here is device-free, allocation-free after Initialize, and
// lock-free. The observation half runs on the FAULTING GUEST THREAD, inside the
// access-violation handler, WITH THE GLOBAL CRITICAL REGION ALREADY HELD --
// PhysicalHeap::TriggerCallbacks walks the callback list under the lock the
// fault path handed it, so the hook takes no lock of its own and must not want
// one. It may do nothing but bounded atomic work: no waiting, no logging, no
// allocation, no new lock, no call back into the command processor beyond the
// two immutable providers.
//
// That already-held lock is also the SYNCHRONIZATION EDGE for the execution
// half. Stamps are written on the GPU thread and read on the faulting guest
// thread, and relaxed atomics alone give no happens-before between them: a
// callback that begins after an enrollment could still legally read the old
// zero stamp and report a genuinely raced page as idle. So the GPU thread
// acquires that SAME lock around the stamp (see
// MetalSharedMemory::RequestConsumedRange). The order is then causal rather
// than hopeful:
//
//   * a callback already holding the lock happened BEFORE the enrollment, and
//     is entitled to see the page idle -- it observed a state that really was
//     current when it ran; and
//   * a callback that acquires the lock after the enrollment released it MUST
//     see the stamp.
//
// The faulting thread takes no new lock for this -- it already holds the only
// one involved -- so its cost is unchanged.
//
// An observer that cannot be armed is NOT a degraded mode. Aliasing removes the
// mirror's snapshot, so an unwatched alias would make "no overlap was reported"
// mean "nobody was looking", which is the absence-as-permission failure this
// whole pre-gate exists to prevent. Initialize therefore returns a verdict the
// owner is required to act on: MetalSharedMemory releases the alias candidate
// and installs the ordinary mirror instead, and the startup report says
// `observer_rejected` rather than claiming an accepted alias.
//
// This is CONSERVATIVE ENROLLMENT, not a rendered-corruption proof. A stamp is
// written at the production request seam, which is where a consumer declares it
// is about to read the range; a draw that later fails, is skipped, or never
// reaches an encoder still leaves the page stamped. So an overlap here is a
// PROMOTION STOP SIGNAL -- "a CPU write raced a page some unfinished submission
// had already claimed" -- and is deliberately not downgraded by trying to prove
// the read really happened. Over-stamping makes the signal too loud, never too
// quiet, which is the only direction a stop signal may err in.

// How one observed host page related to the GPU work that had claimed it.
enum class ZeroCopyOverlapClass : uint32_t {
  // No GPU consumer ever enrolled this page (or the enrollment carried the zero
  // sentinel). A CPU write here races nothing.
  kIdle,
  // The newest enrolling submission is at or below the completed high-water
  // mark, so its reads are finished. Rewriting the page is safe.
  kCompletedReader,
  // The newest enrolling submission has NOT completed. The CPU just rewrote
  // bytes a queued GPU read may still consume -- the promotion stop signal.
  kInFlightReader,
  // Not a value: one past the last real enumerator. Every enumerator must be
  // declared ABOVE this line so the count below, and the conformance arrays
  // pinned to it, follow the enum automatically.
  kCount,
};
inline constexpr uint32_t kZeroCopyOverlapClassCount =
    static_cast<uint32_t>(ZeroCopyOverlapClass::kCount);

// kCount is a bound, never an observation.
constexpr bool IsZeroCopyOverlapClassValue(ZeroCopyOverlapClass value) {
  return static_cast<uint32_t>(value) < kZeroCopyOverlapClassCount;
}

constexpr const char* ZeroCopyOverlapClassName(ZeroCopyOverlapClass value) {
  switch (value) {
    case ZeroCopyOverlapClass::kIdle:
      return "idle";
    case ZeroCopyOverlapClass::kCompletedReader:
      return "completed_reader";
    case ZeroCopyOverlapClass::kInFlightReader:
      return "in_flight_reader";
    case ZeroCopyOverlapClass::kCount:
      break;  // not a value; falls to the out-of-range bucket
  }
  return "unknown";
}

// The whole comparison, as a value. Zero is the idle sentinel and is checked
// FIRST: submission ids start at 1, so an unstamped page compared numerically
// against a completed mark of 0 would otherwise read as a finished reader --
// i.e. a page nobody ever claimed would be reported as safely reusable for the
// same reason a genuinely finished one is, and the two are not the same fact.
//
// The comparison is strict. `page_stamp == completed_submission` means the
// enrolling submission is the newest one the GPU has FINISHED, so its reads are
// done and the rewrite is not a race. Making it `>=` would report every
// just-completed reader as in flight, which is the failure mode that makes an
// overlap counter useless: it would be nonzero on every run, so a real overlap
// could never be distinguished from the noise.
constexpr ZeroCopyOverlapClass ClassifyZeroCopyOverlap(
    uint64_t page_stamp, uint64_t completed_submission) {
  if (!page_stamp) {
    return ZeroCopyOverlapClass::kIdle;
  }
  return page_stamp > completed_submission
             ? ZeroCopyOverlapClass::kInFlightReader
             : ZeroCopyOverlapClass::kCompletedReader;
}

// These counters run for a whole session and are read as evidence. A wrapped
// total would read as a small number -- i.e. as "almost no overlap happened",
// the exact claim this pre-gate has to measure -- so they saturate instead.
constexpr uint64_t ZeroCopySaturatingAdd(uint64_t accumulated, uint64_t added) {
  return added > UINT64_MAX - accumulated ? UINT64_MAX : accumulated + added;
}

// A half-open host-page interval inside the guest physical domain.
struct ZeroCopyPageSpan {
  uint32_t first_page = 0;
  uint32_t page_count = 0;
  // Part (or all) of the requested byte range fell outside the domain. Kept
  // separate from page_count so "nothing to observe" and "the request was
  // bigger than the domain" are distinguishable in the report.
  bool clamped = false;
};

// Overflow-safe conversion of a requested byte range into host pages, clamped
// to the domain. `start + length` is never formed: the distance from `start` to
// the domain end is the only bound that matters and it cannot overflow. The
// physical invalidation callback is driven by guest page faults whose range the
// backend does not choose, so a request that starts past the end, or runs off
// it, is an ordinary input here rather than a defect.
constexpr ZeroCopyPageSpan ClampZeroCopyPageSpan(uint64_t start, uint64_t length,
                                                 uint32_t page_size_log2,
                                                 uint32_t domain_page_count) {
  ZeroCopyPageSpan span;
  // A shift at or past the width of the product below is undefined; reject
  // rather than compute a domain size that means nothing.
  if (!length || !domain_page_count || page_size_log2 >= 32) {
    return span;
  }
  uint64_t domain_bytes = uint64_t(domain_page_count) << page_size_log2;
  if (start >= domain_bytes) {
    span.clamped = true;
    return span;
  }
  uint64_t remaining = domain_bytes - start;
  if (length > remaining) {
    span.clamped = true;
    length = remaining;
  }
  uint64_t first = start >> page_size_log2;
  // start + length <= domain_bytes here, so neither this sum nor the
  // decrement below can wrap.
  uint64_t last = (start + length - 1) >> page_size_log2;
  span.first_page = uint32_t(first);
  span.page_count = uint32_t(last - first + 1);
  return span;
}

// One retained in-flight overlap. Bounded in number and deduplicated by page:
// a title that rewrites the same page every frame would otherwise fill the
// slots with one page's story and hide every other page that overlapped.
struct ZeroCopyOverlapSample {
  uint32_t page_index = 0;
  uint64_t consumer_submission = 0;
  uint64_t completed_submission = 0;
};

inline constexpr uint32_t kZeroCopyOverlapSampleSlots = 8;

// Everything the pre-gate retains. Fixed size: counters plus a fixed number of
// sample slots. No observation is stored individually, so a title that faults
// millions of times costs the same as one that faults once.
struct ZeroCopyOverlapSnapshot {
  // The observer allocated its fixed state, i.e. alias mode installed it. Read
  // from the storage itself, never from a backing-mode flag, so a mirror run
  // that somehow advanced a counter shows up as inactive-but-nonempty instead
  // of being explained away.
  bool active = false;
  uint32_t page_count = 0;
  uint32_t page_size_log2 = 0;

  // EXECUTION source (GPU thread, RequestConsumedRange).
  uint64_t stamp_calls = 0;
  uint64_t stamped_pages = 0;
  // Enrollments that carried the idle sentinel and therefore enrolled NOTHING.
  // Nonzero here means the execution source is not reporting real submissions,
  // so a zero overlap count from this run is not evidence of safety.
  uint64_t zero_submission_stamp_pages = 0;

  // OBSERVATION source (faulting guest thread, physical invalidation thunk).
  uint64_t cpu_write_callbacks = 0;
  uint64_t cpu_write_pages = 0;
  uint64_t clamped_cpu_writes = 0;

  // The classification, one bucket per observed page. The three sum to
  // cpu_write_pages.
  uint64_t idle_pages = 0;
  uint64_t completed_reader_pages = 0;
  uint64_t in_flight_cpu_write_overlap = 0;

  uint32_t sample_count = 0;
  ZeroCopyOverlapSample samples[kZeroCopyOverlapSampleSlots];
};

// When the bounded lifecycle report is due, as a pure function of the SWAP
// FINALIZATION index (task 5.4).
//
// The cadence is deliberately driven by finalizations rather than by successful
// swaps. Every IssueSwap return path finalizes -- including the suppressed
// present, the no-presenter exit and the no-queue exit -- but `swap_count_`
// only advances when a frame actually publishes. A title stuck in persistent
// publication failure is exactly the run whose overlap evidence matters most,
// and keying the report to successful swaps would emit nothing for it at all.
//
// Indices are 1-based, matching the pre-increment at the finalization site, so:
//   * index 0 means no frame boundary has happened yet and is never due;
//   * index 1 is the FIRST boundary and is always due, so one rule covers both
//     "say something as soon as a frame exists" and "keep saying it";
//   * an interval of 0 would be a division by zero and is treated as "never",
//     never as "always" -- a misconfigured cadence must not turn the bounded
//     report into a per-frame log.
constexpr bool ZeroCopyOverlapReportDue(uint64_t finalization_index,
                                        uint64_t interval) {
  if (!finalization_index || !interval) {
    return false;
  }
  return ((finalization_index - 1) % interval) == 0;
}

// The silence rule for the control arms: an exact-off or fallback run must have
// installed nothing AND advanced nothing. Both halves are required, so a run
// that advanced a counter without an installed observer is NOT silent and the
// caller must say so rather than print nothing and call it a clean arm.
constexpr bool ZeroCopyOverlapSnapshotIsSilent(
    const ZeroCopyOverlapSnapshot& in) {
  return !in.active && !in.stamp_calls && !in.stamped_pages &&
         !in.zero_submission_stamp_pages && !in.cpu_write_callbacks &&
         !in.cpu_write_pages && !in.clamped_cpu_writes && !in.idle_pages &&
         !in.completed_reader_pages && !in.in_flight_cpu_write_overlap &&
         !in.sample_count;
}

// Lock-free saturating accumulate. The compare-exchange retries only when
// another thread already succeeded, so it never waits on an external event --
// the constraint that matters on the faulting guest thread.
inline void ZeroCopySaturatingAdd(std::atomic<uint64_t>& counter,
                                  uint64_t added) {
  uint64_t current = counter.load(std::memory_order_relaxed);
  for (;;) {
    uint64_t next = ZeroCopySaturatingAdd(current, added);
    if (next == current) {
      return;  // already saturated (or adding nothing)
    }
    if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return;
    }
  }
}

// Newest reader wins. Two GPU-thread enrollments of the same page must not let
// an older submission overwrite a newer one's claim, and the observation thread
// must never see a stamp go backwards.
inline void ZeroCopyAtomicMax(std::atomic<uint64_t>& slot, uint64_t value) {
  uint64_t current = slot.load(std::memory_order_relaxed);
  while (current < value) {
    if (slot.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return;
    }
  }
}

// The bounded two-source recorder itself. Owns one fixed-size stamp table --
// the guest physical domain divided by the host page size, allocated once at
// Initialize -- plus the counters and sample slots above.
//
// Threading: StampConsumer runs on the GPU thread, ObserveCpuWrite on any
// faulting guest thread, Snapshot on the GPU thread at a lifecycle boundary.
// All three touch only atomics, and none of them takes a lock -- the ORDER
// between them is established by the caller, which runs StampConsumer and
// Snapshot under the same global critical region the invalidation callback
// already holds. Initialize/Shutdown are NOT concurrent with the other three:
// the owner installs the observer before registering the physical invalidation
// callback and releases it only after unregistering it.
class ZeroCopyOverlapObserver {
 public:
  ZeroCopyOverlapObserver() = default;
  ~ZeroCopyOverlapObserver() { Shutdown(); }
  ZeroCopyOverlapObserver(const ZeroCopyOverlapObserver&) = delete;
  ZeroCopyOverlapObserver& operator=(const ZeroCopyOverlapObserver&) = delete;

  // Fixed-size, allocated exactly once. Returns false (and stays inactive) for
  // a domain that does not divide into a usable page count -- an inactive
  // observer is inert, never a partially armed one.
  //
  // The return value is a REQUIRED postcondition of accepting an alias, not
  // advice: see the header comment. `force_rejection` is the diagnostic
  // injection for that rule (kZeroCopyForceObserverRejectEnvVar). A genuine
  // arming failure here is an allocation failure over a 512 MiB domain, which
  // no test can produce on demand, so without an injected one the fallback
  // could only ever be reviewed. It is checked FIRST, so a forced rejection
  // leaves exactly the state a real failure would: shut down, inert, inactive.
  //
  // The table is allocated NON-THROWING and checked, because "returns false" is
  // the whole contract. A throwing allocation would not return false at all: it
  // would unwind past the owner's fallback (or terminate, in a no-exceptions
  // build), which turns a DEFAULT-OFF diagnostic into a fatal failure under the
  // one condition -- host memory pressure -- it is most likely to hit. The
  // fields are also committed only AFTER the allocation and its zeroing
  // succeed, so a refusal leaves the observer with no table and no geometry
  // rather than a page count describing a table that does not exist.
  bool Initialize(uint64_t domain_bytes, uint32_t page_size_log2,
                  bool force_rejection = false) {
    Shutdown();
    if (force_rejection || !domain_bytes || page_size_log2 >= 32) {
      return false;
    }
    uint64_t pages = domain_bytes >> page_size_log2;
    if (!pages || pages > UINT32_MAX) {
      return false;
    }
    uint32_t page_count = uint32_t(pages);
    std::unique_ptr<std::atomic<uint64_t>[]> page_stamps(
        new (std::nothrow) std::atomic<uint64_t>[page_count]);
    if (!page_stamps) {
      return false;
    }
    for (uint32_t i = 0; i < page_count; ++i) {
      page_stamps[i].store(0, std::memory_order_relaxed);
    }
    page_count_ = page_count;
    page_size_log2_ = page_size_log2;
    page_stamps_ = std::move(page_stamps);
    return true;
  }

  void Shutdown() {
    page_stamps_.reset();
    page_count_ = 0;
    page_size_log2_ = 0;
    stamp_calls_.store(0, std::memory_order_relaxed);
    stamped_pages_.store(0, std::memory_order_relaxed);
    zero_submission_stamp_pages_.store(0, std::memory_order_relaxed);
    cpu_write_callbacks_.store(0, std::memory_order_relaxed);
    cpu_write_pages_.store(0, std::memory_order_relaxed);
    clamped_cpu_writes_.store(0, std::memory_order_relaxed);
    idle_pages_.store(0, std::memory_order_relaxed);
    completed_reader_pages_.store(0, std::memory_order_relaxed);
    in_flight_overlap_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kZeroCopyOverlapSampleSlots; ++i) {
      samples_[i].state.store(kSampleEmpty, std::memory_order_relaxed);
    }
  }

  bool active() const { return page_stamps_ != nullptr; }

  // EXECUTION source. `submission` is the id currently being encoded; zero is
  // the idle sentinel and enrolls nothing (but is counted, see
  // zero_submission_stamp_pages).
  void StampConsumer(uint64_t start, uint64_t length, uint64_t submission) {
    if (!active()) {
      return;
    }
    ZeroCopyPageSpan span =
        ClampZeroCopyPageSpan(start, length, page_size_log2_, page_count_);
    if (!span.page_count) {
      return;
    }
    ZeroCopySaturatingAdd(stamp_calls_, 1);
    ZeroCopySaturatingAdd(stamped_pages_, span.page_count);
    if (!submission) {
      // Recording this instead of silently writing the sentinel is what keeps
      // a broken execution source from reading as a clean run.
      ZeroCopySaturatingAdd(zero_submission_stamp_pages_, span.page_count);
      return;
    }
    for (uint32_t i = 0; i < span.page_count; ++i) {
      ZeroCopyAtomicMax(page_stamps_[span.first_page + i], submission);
    }
  }

  // OBSERVATION source. `completed_submission` is supplied by the caller and
  // used for EVERY page of this callback: one high-water mark per physical
  // write, so pages of one write are never classified against different marks.
  void ObserveCpuWrite(uint64_t start, uint64_t length,
                       uint64_t completed_submission) {
    if (!active()) {
      return;
    }
    ZeroCopySaturatingAdd(cpu_write_callbacks_, 1);
    ZeroCopyPageSpan span =
        ClampZeroCopyPageSpan(start, length, page_size_log2_, page_count_);
    if (span.clamped) {
      ZeroCopySaturatingAdd(clamped_cpu_writes_, 1);
    }
    if (!span.page_count) {
      return;
    }
    ZeroCopySaturatingAdd(cpu_write_pages_, span.page_count);
    for (uint32_t i = 0; i < span.page_count; ++i) {
      uint32_t page = span.first_page + i;
      uint64_t stamp = page_stamps_[page].load(std::memory_order_relaxed);
      switch (ClassifyZeroCopyOverlap(stamp, completed_submission)) {
        case ZeroCopyOverlapClass::kIdle:
          ZeroCopySaturatingAdd(idle_pages_, 1);
          break;
        case ZeroCopyOverlapClass::kCompletedReader:
          ZeroCopySaturatingAdd(completed_reader_pages_, 1);
          break;
        case ZeroCopyOverlapClass::kInFlightReader:
          ZeroCopySaturatingAdd(in_flight_overlap_, 1);
          RecordSample(page, stamp, completed_submission);
          break;
        case ZeroCopyOverlapClass::kCount:
          break;  // not a value; never counted as anything
      }
    }
  }

  ZeroCopyOverlapSnapshot Snapshot() const {
    ZeroCopyOverlapSnapshot out;
    out.active = active();
    out.page_count = page_count_;
    out.page_size_log2 = page_size_log2_;
    out.stamp_calls = stamp_calls_.load(std::memory_order_relaxed);
    out.stamped_pages = stamped_pages_.load(std::memory_order_relaxed);
    out.zero_submission_stamp_pages =
        zero_submission_stamp_pages_.load(std::memory_order_relaxed);
    out.cpu_write_callbacks =
        cpu_write_callbacks_.load(std::memory_order_relaxed);
    out.cpu_write_pages = cpu_write_pages_.load(std::memory_order_relaxed);
    out.clamped_cpu_writes =
        clamped_cpu_writes_.load(std::memory_order_relaxed);
    out.idle_pages = idle_pages_.load(std::memory_order_relaxed);
    out.completed_reader_pages =
        completed_reader_pages_.load(std::memory_order_relaxed);
    out.in_flight_cpu_write_overlap =
        in_flight_overlap_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kZeroCopyOverlapSampleSlots; ++i) {
      if (samples_[i].state.load(std::memory_order_acquire) !=
          kSamplePublished) {
        continue;
      }
      out.samples[out.sample_count].page_index = samples_[i].page_index;
      out.samples[out.sample_count].consumer_submission =
          samples_[i].consumer_submission;
      out.samples[out.sample_count].completed_submission =
          samples_[i].completed_submission;
      ++out.sample_count;
    }
    return out;
  }

 private:
  static constexpr uint32_t kSampleEmpty = 0;
  static constexpr uint32_t kSampleClaimed = 1;
  static constexpr uint32_t kSamplePublished = 2;

  struct SampleSlot {
    // Empty -> claimed -> published. The payload is written between the claim
    // and the publish, so a reader that sees `published` sees the whole slot.
    std::atomic<uint32_t> state{kSampleEmpty};
    uint32_t page_index = 0;
    uint64_t consumer_submission = 0;
    uint64_t completed_submission = 0;
  };

  // At most one full pass over the fixed slot array, so the faulting thread's
  // cost is bounded no matter how many overlaps a run produces.
  //
  // Under PRODUCTION concurrency the same-page dedup is EXACT, not best-effort:
  // every call reaching here came through the physical invalidation callback,
  // and those are serialized by the global critical region, so no two of them
  // can be inside this scan at once. The claim/publish protocol below is what
  // covers the remaining case -- a direct caller (conformance drives
  // ObserveCpuWrite without that lock) racing another thread on the same page
  // may take two slots, which costs a slot and never a correctness claim (the
  // COUNTER, not the samples, is the measurement).
  void RecordSample(uint32_t page_index, uint64_t consumer_submission,
                    uint64_t completed_submission) {
    for (uint32_t i = 0; i < kZeroCopyOverlapSampleSlots; ++i) {
      uint32_t state = samples_[i].state.load(std::memory_order_acquire);
      if (state == kSamplePublished && samples_[i].page_index == page_index) {
        return;
      }
      if (state != kSampleEmpty) {
        continue;
      }
      uint32_t expected = kSampleEmpty;
      if (!samples_[i].state.compare_exchange_strong(
              expected, kSampleClaimed, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        continue;  // another thread took this slot; try the next one
      }
      samples_[i].page_index = page_index;
      samples_[i].consumer_submission = consumer_submission;
      samples_[i].completed_submission = completed_submission;
      samples_[i].state.store(kSamplePublished, std::memory_order_release);
      return;
    }
  }

  std::unique_ptr<std::atomic<uint64_t>[]> page_stamps_;
  uint32_t page_count_ = 0;
  uint32_t page_size_log2_ = 0;

  std::atomic<uint64_t> stamp_calls_{0};
  std::atomic<uint64_t> stamped_pages_{0};
  std::atomic<uint64_t> zero_submission_stamp_pages_{0};
  std::atomic<uint64_t> cpu_write_callbacks_{0};
  std::atomic<uint64_t> cpu_write_pages_{0};
  std::atomic<uint64_t> clamped_cpu_writes_{0};
  std::atomic<uint64_t> idle_pages_{0};
  std::atomic<uint64_t> completed_reader_pages_{0};
  std::atomic<uint64_t> in_flight_overlap_{0};
  SampleSlot samples_[kZeroCopyOverlapSampleSlots];
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ZERO_COPY_OVERLAP_H_
