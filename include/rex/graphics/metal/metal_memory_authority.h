/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_MEMORY_AUTHORITY_H_
#define XENIA_GPU_METAL_METAL_MEMORY_AUTHORITY_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "rex/graphics/metal/metal_guest_range.h"

namespace rex {
namespace graphics {
namespace metal {

// Typed, non-owning cross-writer memory-authority decision and execution seam
// (OpenSpec implement-metal-cross-writer-memory-authority).
//
// This is the coordination contract the audit calls for: the same pending
// producer state (pending_writebacks_ / pending_memexports_) was previously
// interpreted independently by the vertex, texture, and WAIT_REG_MEM call
// sites. This header gives every promoted writer and consumer one typed
// vocabulary and one pure decision function for selective convergence.
//
// Design constraints this file honors (docs/metal_cross_writer_memory_authority
// _audit.md):
//   * Device-free and pure. No Metal, no command processor, no device handle.
//   * Non-owning. The decision functions read a SNAPSHOT of the existing
//     pending-producer records; they never store, mutate, or own them, and add
//     NO second version bitmap, address map, cache-key map, or independent
//     pending queue. ::rex::gpu::SharedMemory validity and the command processor's pending
//     queues remain the sole sources of truth.
//   * Range-set aware. Consumers supply their precise GuestRangeSet (a texture's
//     disjoint base/mip extents, an aliased/multi-consumer set); the decision
//     selects ONLY intersecting producer records. Adjacent/disjoint ranges
//     choose no dependency (GuestRange treats adjacency as non-overlap).
//   * Non-authoritative execution. The RAM-writer publication and consumer
//     dependency seams below execute the existing publication/drain operations
//     for the promoted writers and consumers. They own no authority state.

// Which of the four writers produced (or will produce) the bytes.
enum class WriterKind {
  kGuestCpu,      // guest CPU store; RAM is authoritative, published by the
                  // physical-memory invalidation callback.
  kResolve,       // color/depth resolve scatter to guest RAM.
  kMemexport,     // shader memory export into the shared-memory buffer.
  kTraceRestore,  // trace playback restoring bytes directly to guest RAM.
};

// Which host/GPU consumer is preparing to read the range.
enum class ConsumerKind {
  kTexture,      // texture fetch (reads shared-memory / RAM-derived content).
  kVertex,       // vertex/index fetch (reads the shared-memory buffer).
  kWait,         // WAIT_REG_MEM and memory-predicate polls (reads guest RAM).
  kHostCpuRead,  // any other audited host CPU read of guest RAM.
  // Memexport destination preparation. It is listed here, with the readers,
  // because that is literally what it does: RequestRange uploads guest RAM into
  // the shared buffer for the destination pages, so the same "an older producer
  // still owes these bytes" question applies to it verbatim. What makes it its
  // own kind rather than a reuse of kVertex is the second half -- the pages it
  // requests are about to be overwritten by a GPU store that must be the NEWEST
  // writer of them. See DecideMemexportDestinationOrdering.
  kMemexportDestination,
  // Sentinel, not a kind: bounds checked indexing (seam timing) and pins the
  // completeness tests. ConsumerKindName deliberately does NOT name it.
  kCount,
};

// The bounded authority state of a range. RAM-current is the steady state; the
// two transit states are the only sanctioned GPU-newer-than-RAM conditions, and
// each MUST be owned by a submission-scoped pending record.
enum class TransitState {
  kRamCurrent,       // guest RAM holds the guest-visible bytes.
  kPendingResolve,   // a completed/queued resolve owes RAM its scattered bytes.
  kPendingMemexport,  // the shared buffer holds newer bytes owed to RAM.
  // Stage B (implement-metal-gpu-visible-resolve-authority): a GPU scatter
  // already placed the newest bytes in the SHARED BUFFER at the guest physical
  // offset. Guest RAM is stale until published, so this is still a transit
  // state owing RAM its bytes -- the difference is WHO can be served without
  // waiting: a consumer reading the shared buffer can read these bytes under
  // ordering alone, while a consumer reading guest RAM must still await and
  // drain exactly as for kPendingResolve.
  //
  // A record in this state is NEVER retired by a shared-buffer consumer. Only
  // RAM publication retires it, which is what preserves demand CPU visibility.
  kGpuCurrentShared,
};

// Which memory a particular consumer request will actually read. This is NOT
// derivable from ConsumerKind: the legacy texture untile reads guest RAM
// through TranslatePhysical, while the routed load-shader path reads the
// shared buffer for the SAME kTexture kind. Keying the decision on the kind
// would therefore serve a RAM reader from GPU-current bytes it cannot see.
// Callers declare what they are about to do; the default is the conservative
// one, so every pre-Stage-B call site keeps its exact behavior.
enum class ConsumerAccess {
  kGuestRam,      // reads guest RAM: GPU-current bytes must be published first.
  kSharedBuffer,  // reads the shared buffer: GPU-current bytes are readable
                  // under ordering, with no completion wait and no drain.
};

// The action the authority contract selects. These match the design's explicit
// action set (design.md Decision 1 / Decision 5).
enum class AuthorityAction {
  kNoDependency,            // consumer disjoint from every pending producer:
                           // no commit, wait, or drain.
  kAwaitProducer,          // an overlapping producer's owning submission has not
                           // completed; await it, then drain.
  kDrainMatchingRecords,   // overlapping producers are complete; converge only
                           // the intersecting records to RAM.
  kInvalidateRamWrite,     // a RAM writer publishes its final extent by
                           // invalidating shared pages + overlapping caches.
  kRequestValidatedGpuBytes,  // a shared-buffer consumer revalidates its exact
                              // bytes from RAM after producers converged.
  kPublishGpuWrite,        // memexport marks the range GPU-written and registers
                           // its pending record as ONE ordered transition.
  kConvergeRamFromBuffer,  // a completed memexport copies buffer->RAM and
                           // retires its record without clobbering the buffer.
  kOrderAgainstGpuProducers,  // Stage B: every selected producer is
                              // GPU-current and this consumer reads the shared
                              // buffer -- satisfy it by ordering the consuming
                              // work after the producing submission, with no
                              // completion wait and no drain.
};

// A non-owning snapshot of one pending GPU producer record as it currently
// exists in pending_writebacks_ / pending_memexports_. The seam reads these to
// decide dependencies; it never owns or mutates the underlying record.
struct PendingProducer {
  GuestRange range;
  TransitState state;   // kPendingResolve or kPendingMemexport.
  uint64_t submission;  // owning submission id (0 == pre-submission / S7-off).
  uint64_t sequence = 0;  // chronological writer registration order.
};

// True once a producer's owning submission has finished on the GPU, using the
// same completed-submission high-water mark the command processor tracks.
inline bool ProducerComplete(const PendingProducer& producer,
                             uint64_t completed_submission) {
  // Submission 0 means "no owning submission tag". Since the async promotion
  // (2026-08-09) that is NO LONGER the default shape: MetalCommandProcessor::
  // S7TagsOff() is false unless XE_METAL_ASYNC=0 or an explicit S7 kill switch
  // is set, so the publication seams register real submission ids and the
  // ordinary record IS decidable from its own tag. Untagged is now the rollback
  // and pre-submission shape.
  //
  // The rule below is unchanged BY DESIGN, and the inversion is exactly why it
  // must stay unchanged: an untagged record is still not decidable from its tag
  // and must still cross the synchronous commit+wait boundary before RAM
  // convergence. Treating 0 <= 0 as completion here would make every untagged
  // record unconditionally publishable, including one whose commit never
  // happened. The boundary proof is supplied separately, as
  // MemexportBoundaryEvidence, and never inferred inside this predicate.
  return producer.submission != 0 &&
         producer.submission <= completed_submission;
}

// True when a consumer reads the shared-memory buffer (and therefore must
// revalidate its exact bytes from RAM after any dependency resolves). Wait and
// host-CPU reads read guest RAM directly and never revalidate GPU bytes.
inline bool ConsumerReadsSharedBuffer(ConsumerKind consumer) {
  return consumer == ConsumerKind::kTexture ||
         consumer == ConsumerKind::kVertex ||
         consumer == ConsumerKind::kMemexportDestination;
}

// The full decision for one consumer preparing one requested range set against
// the current pending-producer snapshot. `selected` holds ONLY the producers
// that actually intersect the request, in snapshot order -- the selective-drain
// set. Disjoint/adjacent producers are never selected.
struct ConsumerDecision {
  ConsumerKind consumer = ConsumerKind::kTexture;
  AuthorityAction action = AuthorityAction::kNoDependency;
  // Filled by DecideConsumer below. The command processor's indexed and
  // fast-empty paths leave it empty (XE_METAL_LEAN_DECISION, default on):
  // nothing in production reads it.
  GuestRangeSet requested;
  std::vector<PendingProducer> selected;
  bool await = false;                  // any selected producer incomplete.
  bool drain = false;                  // at least one producer to converge.
  bool revalidate_gpu_bytes = false;   // consumer reads the shared buffer.
  uint64_t max_wait_submission = 0;    // highest submission to await (0 if none).
  // Stage B. `requires_gpu_ordering` means at least one selected producer is
  // GPU-current and served to this consumer without a wait: the caller MUST
  // order its consuming work after `max_order_submission` (encoder ordering /
  // fence), which is NOT a CPU wait. Records served this way stay pending --
  // they are not drained and not retired here.
  bool requires_gpu_ordering = false;
  uint64_t max_order_submission = 0;
};

// Producer-state-specific request ranges. Most consumers read the same ranges
// regardless of writer state and use Uniform(). Texture bridge service is the
// one factual exception: a bridge-served resolve is already GPU-ordered, while
// memexport at that address still needs convergence.
struct ConsumerRequest {
  GuestRangeSet resolve_ranges;
  GuestRangeSet memexport_ranges;
  // Which memory this request will read. Defaults to the conservative
  // guest-RAM domain, so a caller that does not know (or predates Stage B)
  // gets exactly the pre-Stage-B decision.
  ConsumerAccess access = ConsumerAccess::kGuestRam;
  // Stage B endgame opt-in: treat kPendingMemexport producers as
  // ordering-servable for a shared-buffer consumer. Memexport output lands
  // in the shared buffer at the guest physical offset at EXECUTION time —
  // the same residency as kGpuCurrentShared (its RAM convergence is a plain
  // memcpy precisely because of that) — so a shared-buffer reader needs
  // ordering, not completion. Default false: every pre-existing call site
  // and conformance case keeps the historical mixed-set fallback.
  bool order_memexport_producers = false;

  static ConsumerRequest Uniform(const GuestRangeSet& ranges) {
    return {ranges, ranges};
  }

  const GuestRangeSet& ForProducer(TransitState state) const {
    return state == TransitState::kPendingMemexport ? memexport_ranges
                                                    : resolve_ranges;
  }

  GuestRangeSet Combined() const {
    GuestRangeSet combined = resolve_ranges;
    for (const GuestRange& range : memexport_ranges.ranges()) {
      combined.Add(range);
    }
    combined.Normalize();
    return combined;
  }
};

// The pure consumer decision. Seeds selection only from producers intersecting
// `requested`, then closes over older overlapping predecessors so whole-record
// convergence cannot invert writer chronology. Classifies the resulting action
// and reports whether await/drain/revalidate is required. No side effects, no
// ownership, no device.
inline ConsumerDecision DecideConsumer(
    ConsumerKind consumer, const ConsumerRequest& request,
    const std::vector<PendingProducer>& producers,
    uint64_t completed_submission) {
  ConsumerDecision decision;
  decision.consumer = consumer;
  decision.requested = request.Combined();
  decision.revalidate_gpu_bytes = ConsumerReadsSharedBuffer(consumer);
  // Fast path, exact: with no intersecting producer nothing is seeded, the
  // closure adds nothing, and the result is the default no-dependency
  // decision. Most per-draw texture/vertex queries end here (JC2: 74% of
  // ~6,300 queries per frame), so skip the selection bookkeeping entirely.
  bool any_seed = false;
  for (const PendingProducer& producer : producers) {
    if (request.ForProducer(producer.state).OverlapsAny(producer.range)) {
      any_seed = true;
      break;
    }
  }
  if (!any_seed) {
    return decision;
  }
  std::vector<bool> selected(producers.size(), false);
  for (size_t i = 0; i < producers.size(); ++i) {
    const PendingProducer& producer = producers[i];
    if (!request.ForProducer(producer.state).OverlapsAny(producer.range)) {
      continue;  // disjoint or merely adjacent: not a dependency.
    }
    selected[i] = true;
  }

  // Convergence applies whole producer records, not only their intersection
  // with the consumer request. Close the selected set transitively over every
  // older overlapping producer: otherwise an unselected predecessor could be
  // drained later and overwrite a newer record already converged here. This is
  // especially important for bridge service, where the initial request may
  // omit resolves, but it is writer-kind-independent (M1 -> R2 -> M3 chains
  // need all three). A frontier makes the closure O(selected * producers), and
  // resolve-only bridge traffic keeps its fast path because it seeds nothing.
  std::vector<size_t> closure_frontier;
  closure_frontier.reserve(producers.size());
  for (size_t i = 0; i < producers.size(); ++i) {
    if (selected[i]) {
      closure_frontier.push_back(i);
    }
  }
  for (size_t frontier_index = 0; frontier_index < closure_frontier.size();
       ++frontier_index) {
    const PendingProducer& newer =
        producers[closure_frontier[frontier_index]];
    for (size_t older_index = 0; older_index < producers.size();
         ++older_index) {
      const PendingProducer& older = producers[older_index];
      if (selected[older_index] || older.sequence >= newer.sequence ||
          !older.range.Overlaps(newer.range)) {
        continue;
      }
      selected[older_index] = true;
      closure_frontier.push_back(older_index);
    }
  }

  // A GPU-current record is satisfiable by ordering ONLY for a consumer that
  // reads the shared buffer, where those bytes already live. Every other
  // combination keeps the historical await/drain semantics, which is what
  // preserves demand CPU visibility: a guest-RAM reader of a GPU-current range
  // still waits for it and still drains it to RAM.
  // ALL-OR-NOTHING, deliberately. The ordering path is taken only when every
  // selected record is GPU-current and this consumer reads the shared buffer.
  //
  // A per-record split is NOT safe here: `selected` is also the DRAIN set, and
  // draining publishes a record to RAM. Mixing the two policies in one
  // decision would publish a GPU-current record that was never waited for,
  // copying bytes the GPU may still be writing. Falling back to the historical
  // await/drain for the whole set is always correct, merely slower, so the
  // mixed case keeps it.
  bool all_gpu_current = request.access == ConsumerAccess::kSharedBuffer;
  for (size_t i = 0; i < producers.size() && all_gpu_current; ++i) {
    if (!selected[i]) {
      continue;
    }
    if (producers[i].state == TransitState::kGpuCurrentShared) {
      continue;
    }
    if (producers[i].state == TransitState::kPendingMemexport &&
        request.order_memexport_producers) {
      continue;  // opt-in: buffer-resident like kGpuCurrentShared.
    }
    all_gpu_current = false;
  }
  for (size_t i = 0; i < producers.size(); ++i) {
    if (!selected[i]) {
      continue;
    }
    const PendingProducer& producer = producers[i];
    decision.selected.push_back(producer);
    if (all_gpu_current) {
      // Ordering, not waiting. The record is deliberately left pending: only
      // RAM publication retires it, so a later guest-RAM consumer of the same
      // range still gets correct bytes.
      decision.requires_gpu_ordering = true;
      if (producer.submission > decision.max_order_submission) {
        decision.max_order_submission = producer.submission;
      }
      continue;
    }
    if (!ProducerComplete(producer, completed_submission)) {
      decision.await = true;
      if (producer.submission > decision.max_wait_submission) {
        decision.max_wait_submission = producer.submission;
      }
    }
  }
  std::stable_sort(decision.selected.begin(), decision.selected.end(),
                   [](const PendingProducer& a, const PendingProducer& b) {
                     return a.sequence < b.sequence;
                   });
  if (decision.selected.empty()) {
    decision.action = AuthorityAction::kNoDependency;
    decision.drain = false;
  } else if (all_gpu_current) {
    // Every selected record is GPU-current and this consumer reads the shared
    // buffer: no wait, and NO DRAIN -- draining here would retire records that
    // guest RAM has not received yet.
    decision.drain = false;
    decision.action = AuthorityAction::kOrderAgainstGpuProducers;
  } else {
    decision.drain = true;
    decision.action = decision.await ? AuthorityAction::kAwaitProducer
                                     : AuthorityAction::kDrainMatchingRecords;
  }
  return decision;
}

inline ConsumerDecision DecideConsumer(
    ConsumerKind consumer, const GuestRangeSet& requested,
    const std::vector<PendingProducer>& producers,
    uint64_t completed_submission) {
  return DecideConsumer(consumer, ConsumerRequest::Uniform(requested),
                        producers, completed_submission);
}

// The pure writer-publication decision: how a writer's newly-final bytes reach
// their steady authority. Guest CPU, resolve, and trace-restore publish RAM
// (invalidate shared pages + caches); memexport publishes a GPU-written transit
// range owned by its pending record.
inline AuthorityAction DecideWriterPublication(WriterKind writer) {
  switch (writer) {
    case WriterKind::kMemexport:
      return AuthorityAction::kPublishGpuWrite;
    case WriterKind::kGuestCpu:
    case WriterKind::kResolve:
    case WriterKind::kTraceRestore:
    default:
      return AuthorityAction::kInvalidateRamWrite;
  }
}

// The transit state a writer's range occupies while a pending record owns it
// (kRamCurrent for the RAM writers whose publication is immediate).
inline TransitState WriterTransitState(WriterKind writer) {
  switch (writer) {
    case WriterKind::kResolve:
      return TransitState::kPendingResolve;
    case WriterKind::kMemexport:
      return TransitState::kPendingMemexport;
    case WriterKind::kGuestCpu:
    case WriterKind::kTraceRestore:
    default:
      return TransitState::kRamCurrent;
  }
}

inline const char* WriterKindName(WriterKind w) {
  switch (w) {
    case WriterKind::kGuestCpu: return "guest_cpu";
    case WriterKind::kResolve: return "resolve";
    case WriterKind::kMemexport: return "memexport";
    case WriterKind::kTraceRestore: return "trace_restore";
    default: return "?";
  }
}
inline const char* ConsumerKindName(ConsumerKind c) {
  switch (c) {
    case ConsumerKind::kTexture: return "texture";
    case ConsumerKind::kVertex: return "vertex";
    case ConsumerKind::kWait: return "wait";
    case ConsumerKind::kHostCpuRead: return "host_cpu_read";
    case ConsumerKind::kMemexportDestination: return "memexport_destination";
    default: return "?";
  }
}
// Factual writer-side publication result returned by PublishRamWrite. `action`
// is the promoted outcome the caller checks (kInvalidateRamWrite on success);
// the remaining fields report which existing invalidation operations actually
// ran so callers report facts rather than infer them.
struct RamWritePublicationOutcome {
  AuthorityAction action = AuthorityAction::kNoDependency;
  uint32_t invalidated_entries = 0;
  bool shared_invalidation_executed = false;
  bool legacy_invalidation_executed = false;
};

// Factual result of producer convergence used by the promoted selective-drain
// executor and its behavioral tests.
struct ProducerDrainOutcome {
  uint32_t drained_records = 0;
  uint32_t resolve_records = 0;
  uint32_t memexport_records = 0;
  uint32_t invalidated_entries = 0;
  // Bytes memexport convergence physically copied while draining, summed from
  // each MemexportConvergenceResult. Retirement is counted by `memexport_records`
  // and is deliberately NOT a function of this: an aliased backing retires the
  // same records having moved zero bytes.
  uint64_t memexport_bytes_copied = 0;
};

// The one publication-convergence predicate shared by production and the
// conformance harness: a RAM consumer's publication is complete only when the
// selective drain retired exactly one pending record for every selected
// producer. A short retirement (the stop-on-first-failure policy above halting,
// or a failed wait yielding an empty drain) leaves the failing record and its
// untouched selected suffix pending, so the consumer must fail closed.
//
// MetalCommandProcessor::ResolvePendingProducerDependencies uses this to set
// ProducerDependencyOutcome::complete, so the bool the swap's full-domain
// publication consumes as its present/suppress gate is exactly this function of
// the decision and drain. Owning no state, it lets the device-free tests assert
// the SAME gate production presents on, rather than an independently recomputed
// availability flag.
inline bool ConsumerPublicationComplete(const ConsumerDecision& decision,
                                        const ProducerDrainOutcome& drain) {
  return drain.drained_records == decision.selected.size();
}

// The selective-drain executor shared by production
// (MetalCommandProcessor::DrainSelectedPendingProducers) and the task-4.5
// conformance executors. It walks the seam's sequence-ordered selected records,
// drains each through the caller-supplied one-record callback, merges the
// factual ProducerDrainOutcome counts, and stops the moment a record fails to
// retire exactly one pending record.
//
// Stopping on the first non-unit retirement preserves authority chronology: a
// retained older record must never be followed by a newer selected writer, or a
// later retry of the older record could overwrite the newer bytes and invert
// writer order. The caller fails the consumer closed while the failing record
// and the untouched selected suffix stay pending.
//
// This helper owns NO state and adds no queue, flag, ownership, or title rule.
// The callback performs the actual convergence against whatever pending queue it
// closes over, matching each selected record back by its unique authority
// sequence; the selection itself remains a transient value.
template <typename DrainOneProducer>
inline ProducerDrainOutcome DrainSelectedProducers(
    const ConsumerDecision& decision, DrainOneProducer&& drain_one) {
  ProducerDrainOutcome combined;
  for (const PendingProducer& producer : decision.selected) {
    ProducerDrainOutcome one = drain_one(producer);
    combined.drained_records += one.drained_records;
    combined.resolve_records += one.resolve_records;
    combined.memexport_records += one.memexport_records;
    combined.invalidated_entries += one.invalidated_entries;
    combined.memexport_bytes_copied += one.memexport_bytes_copied;
    if (one.drained_records != 1) {
      break;
    }
  }
  return combined;
}

// RAM-write publication operations, exposed through a thin device-free
// port so the typed seam can execute them and tests can substitute a recorder.
// The port exposes invalidation only -- never watch registration or authority
// storage -- so routing through it cannot add a second CPU watch.
struct RamWritePublicationTarget {
  virtual ~RamWritePublicationTarget() = default;

  // Invalidate the shared-memory pages backing [base, base+length).
  virtual bool InvalidateSharedPages(uint32_t base, uint32_t length,
                                     bool exact_range) = 0;

  // Invalidate overlapping legacy texture-cache entries, returning the number
  // retired. Only writers whose policy requires the independently-needed direct
  // invalidation (trace restore, whose legacy global watch is unregistered by
  // SetTracePlayback) call it. Task 5.3's writer-matrix comparison proved this
  // leg is durably required, not a temporary phase-5 measure: with the global
  // watch gone the watch fan-out is empty, so nothing else retires these
  // entries.
  virtual bool InvalidateLegacyTextures(uint32_t base, uint32_t length,
                                        uint32_t* invalidated_entries) = 0;
};

// Source-appropriate RAM-write publication policy. The sole physical-memory
// callback already performs guest-CPU publication, so a typed model decision
// must not invalidate the same write a second time.
struct RamWritePublicationPolicy {
  bool execute_shared_invalidation = true;
  bool exact_range = true;
  bool execute_legacy_invalidation = false;
};

inline RamWritePublicationPolicy RamWritePolicyFor(WriterKind writer) {
  switch (writer) {
    case WriterKind::kGuestCpu:
      // The sole physical callback has already invalidated shared pages and
      // fired watches; the typed policy executes no duplicate operation.
      return {/*shared=*/false, /*exact=*/false, /*legacy=*/false};
    case WriterKind::kTraceRestore:
      // Exact publication plus the independently-required direct legacy texture
      // invalidation. Trace playback unregisters the legacy global watch, so
      // watch fan-out cannot retire these entries and the direct InvalidateRange
      // is durably required. Task 5.3's writer-matrix comparison proved this
      // leg is not removable (the global-watch set is empty after
      // SetTracePlayback), so it is retained by design rather than pending a
      // later equivalence proof.
      return {/*shared=*/true, /*exact=*/true, /*legacy=*/true};
    case WriterKind::kResolve:
      // Exact publication of the checked scatter extent. The resolve drain
      // performs the scoped direct legacy RAM-upload invalidation separately;
      // this publication suppresses duplicate global fan-out while still
      // firing shared-cache per-range watches. Routed in task 3.2; the policy
      // lives here so the seam speaks one vocabulary across writers.
      return {/*shared=*/true, /*exact=*/true, /*legacy=*/false};
    case WriterKind::kMemexport:
    default:
      // Memexport does not publish RAM by invalidation; it converges RAM from
      // the buffer under its pending record (task 3.3). The seam performs no RAM
      // invalidation for it.
      return {/*shared=*/false, /*exact=*/true, /*legacy=*/false};
  }
}

// --- Memexport destination preparation / publication / convergence (task 3.3)
//
// Memexport is the one writer whose bytes exist in the shared buffer before
// they exist in guest RAM (design.md Decision 2's "pending memexport" transit
// state). Three distinct typed steps route through this seam:
//   1. Destination preparation: retire every OLDER overlapping pending producer
//      and then request the destination pages, both BEFORE the GPU write, so
//      neither a stale CPU-RAM upload nor a later drain of an older writer can
//      land after the export.
//   2. Publication: after preflighting the marker, the seam registers the
//      submission-scoped pending record first and then calls RangeWrittenByGpu
//      in the SAME ordered step. No interval is marked GPU-newer before it has
//      an owner.
//   3. Convergence: a completed pending record's exact bytes are copied
//      shared-buffer -> RAM. The port never touches shared-buffer validity;
//      the buffer remains the authoritative source the copy read from. The
//      caller retires the pending record only when the copy reports success.

// Requests the destination range's pages before the GPU write.
struct MemexportPreparationTarget {
  virtual ~MemexportPreparationTarget() = default;
  virtual bool RequestDestinationRange(uint32_t base, uint32_t length) = 0;
};

// One normalized memexport destination-preparation decision.
struct MemexportDestinationPreparation {
  GuestRange range;
};

inline std::optional<MemexportDestinationPreparation>
DecideMemexportDestinationPreparation(uint32_t base, uint32_t length) {
  std::optional<GuestRange> range = GuestRange::FromBaseLength(base, length);
  if (!range) {
    return std::nullopt;
  }
  return MemexportDestinationPreparation{*range};
}

// Executes the PAGE-REQUEST STEP of destination preparation through the
// existing ::rex::gpu::SharedMemory::RequestRange operation. Returns the factual
// RequestRange result -- false means the exports have nowhere to land and the
// caller must treat the draw the same as today (skip it), never invent success.
//
// This is a step, not the whole preparation. A producer path must reach it
// through PrepareOrderedMemexportDestinations below, which runs the
// writer-before-writer ordering FIRST and never issues the request when that
// ordering is unsatisfied. Calling this on its own requests pages for a store
// whose predecessors may still be pending.
inline bool PrepareMemexportDestination(
    const MemexportDestinationPreparation& preparation,
    MemexportPreparationTarget& target) {
  return target.RequestDestinationRange(preparation.range.start(),
                                        preparation.range.length());
}

// --- Writer-before-writer ordering for a memexport destination --------------
//
// A memexport store is the NEWEST writer of the bytes it exports. Every pending
// producer that already owns overlapping bytes is therefore OLDER, and must be
// converged and retired BEFORE the store is encoded and registered -- otherwise
// that older record is drained afterwards and writes ITS bytes over the export,
// inverting writer chronology.
//
// The inversion is not hypothetical, and it is not symmetric across backings.
// A pending RESOLVE owns bytes that live in host staging, OUTSIDE the shared
// buffer, and are owed to guest RAM. When the shared buffer is a separate
// mirror, a late resolve drain writes RAM and the export's own convergence then
// copies buffer -> RAM over it, so the newest writer still wins by accident.
// When the shared buffer ALIASES guest RAM the accident stops working: the
// export already IS in RAM, the late resolve overwrites it there, and memexport
// convergence is a self-copy that has nothing left to restore. The bytes are
// gone. Ordering the writers at the destination seam is what makes the outcome
// a property of the model instead of a property of the backing.
//
// Selection reuses DecideConsumer verbatim -- the same intersection test and
// the same transitive closure over older overlapping predecessors that every
// other barrier in this backend uses. Two consequences are load-bearing:
//   * a DISJOINT (or merely adjacent) producer is never selected, so this seam
//     cannot become a global drain of unrelated writers; and
//   * an empty selection is a MEASURED absence -- the snapshot was consulted
//     and contained no overlapping producer -- which is why it gets its own
//     verdict below instead of sharing "not evaluated" with the case where no
//     snapshot was ever read.

// Whether the ordering precondition was established, and how.
enum class MemexportDestinationOrdering : uint32_t {
  // No ordering decision was made. The DEFAULT, and never a licence: a
  // default-constructed outcome must not read as a prepared destination. Also
  // the verdict for an EMPTY destination set -- no range was compared against
  // the snapshot, so nothing was established and the absence of destinations
  // grants nothing.
  kNotEvaluated,
  // The pending-producer snapshot was consulted and selected nothing. Nothing
  // older overlaps the destination, so the store is already last.
  kNoOlderProducer,
  // Older overlapping producers existed and every one of them retired.
  kOlderProducersRetired,
  // Older overlapping producers existed, every one of them is GPU-resident
  // (a GPU-current resolve or a pending memexport in the shared buffer) and
  // the store is encoded behind them on the same queue: writer chronology is
  // carried by GPU order, the records stay pending, no CPU wait was spent.
  kOlderProducersGpuOrdered,
  // Older overlapping producers existed and at least one is STILL PENDING. The
  // destination is not prepared, the pages are not requested, and the caller
  // must fail the draw closed -- encoding the store anyway would leave the
  // retained record free to overwrite it later.
  kOlderProducersRetained,
  // Not a value: one past the last real enumerator.
  kCount,
};
inline constexpr uint32_t kMemexportDestinationOrderingCount =
    static_cast<uint32_t>(MemexportDestinationOrdering::kCount);

inline constexpr std::array kMemexportDestinationOrderings = {
    MemexportDestinationOrdering::kNotEvaluated,
    MemexportDestinationOrdering::kNoOlderProducer,
    MemexportDestinationOrdering::kOlderProducersRetired,
    MemexportDestinationOrdering::kOlderProducersGpuOrdered,
    MemexportDestinationOrdering::kOlderProducersRetained,
};
static_assert(kMemexportDestinationOrderings.size() ==
                  kMemexportDestinationOrderingCount,
              "kMemexportDestinationOrderings must enumerate every real "
              "MemexportDestinationOrdering value exactly once");

constexpr bool IsMemexportDestinationOrderingValue(
    MemexportDestinationOrdering value) {
  return static_cast<uint32_t>(value) < kMemexportDestinationOrderingCount;
}

// The one place that decides whether a verdict licenses the store. Exhaustive
// with NO `default:`, so a new enumerator is a compiler warning here rather
// than silently inheriting someone's notion of "ordered"; kCount and any
// out-of-range value fall through to false, because a value the vocabulary does
// not define can never license a write.
constexpr bool MemexportDestinationOrderingSatisfied(
    MemexportDestinationOrdering value) {
  switch (value) {
    case MemexportDestinationOrdering::kNotEvaluated:
    case MemexportDestinationOrdering::kOlderProducersRetained:
      return false;
    case MemexportDestinationOrdering::kNoOlderProducer:
    case MemexportDestinationOrdering::kOlderProducersRetired:
    case MemexportDestinationOrdering::kOlderProducersGpuOrdered:
      return true;
    case MemexportDestinationOrdering::kCount:
      break;  // not a value
  }
  return false;
}

constexpr const char* MemexportDestinationOrderingName(
    MemexportDestinationOrdering value) {
  switch (value) {
    case MemexportDestinationOrdering::kNotEvaluated:
      return "not_evaluated";
    case MemexportDestinationOrdering::kNoOlderProducer:
      return "no_older_producer";
    case MemexportDestinationOrdering::kOlderProducersRetired:
      return "older_producers_retired";
    case MemexportDestinationOrdering::kOlderProducersGpuOrdered:
      return "older_producers_gpu_ordered";
    case MemexportDestinationOrdering::kOlderProducersRetained:
      return "older_producers_retained";
    case MemexportDestinationOrdering::kCount:
      break;  // not a value
  }
  return "unknown";
}

// The pure ordering decision for ONE draw's whole destination set. The union is
// taken first so a draw exporting several ranges resolves its predecessors at a
// single barrier: preparing the ranges one at a time would let the drain of a
// later range invalidate pages an earlier range had already requested.
//
// `gpu_ordered`: the store executes on the same queue as every GPU-resident
// producer, so when ALL older overlapping producers are GPU-resident
// (kGpuCurrentShared, or kPendingMemexport) the decision is
// kOrderAgainstGpuProducers: no wait, no drain. The shared buffer is the single
// source of those bytes and the export overwrites it in queue order, so the
// newest writer wins in the buffer, and every later RAM publication (eager
// drain or demand fault) copies from the buffer. A host-staged pending resolve
// in the set breaks the ordering and the historical await/drain applies.
inline ConsumerDecision DecideMemexportDestinationOrdering(
    const std::vector<MemexportDestinationPreparation>& preparations,
    const std::vector<PendingProducer>& producers,
    uint64_t completed_submission, bool gpu_ordered = false) {
  GuestRangeSet destinations;
  for (const MemexportDestinationPreparation& preparation : preparations) {
    destinations.Add(preparation.range);
  }
  destinations.Normalize();
  ConsumerRequest request = ConsumerRequest::Uniform(destinations);
  if (gpu_ordered) {
    request.access = ConsumerAccess::kSharedBuffer;
    request.order_memexport_producers = true;
  }
  return DecideConsumer(ConsumerKind::kMemexportDestination, request,
                        producers, completed_submission);
}

// Everything the ordered preparation established, as facts rather than as a
// bool the caller has to interpret.
struct MemexportDestinationPreparationOutcome {
  MemexportDestinationOrdering ordering =
      MemexportDestinationOrdering::kNotEvaluated;
  // What the snapshot actually selected, retained so a caller (and a test) can
  // report WHICH older producers blocked the store rather than only that some
  // did.
  ConsumerDecision decision;
  // What retiring them actually achieved. Zeroed when nothing was selected.
  ProducerDrainOutcome drain;
  uint32_t range_count = 0;
  uint32_t requested_ranges = 0;
  // Every destination range was requested successfully. False whenever the
  // ordering blocked the requests, so it can never read as "the pages are
  // ready" for a store that was not licensed.
  bool requested = false;

  bool ordered() const {
    return MemexportDestinationOrderingSatisfied(ordering);
  }
  // The single predicate the producer path gates on.
  bool prepared() const { return ordered() && requested; }
};

// The whole destination preparation: order the writers, THEN request the pages.
//
// `retire_older` is handed the selection and must converge and retire exactly
// those records, returning the factual drain. It is invoked ONLY when something
// was selected, which is what keeps a disjoint or empty snapshot from costing a
// commit/wait. Completeness is judged by ConsumerPublicationComplete -- the
// same predicate every other consumer barrier fails closed on -- so a short
// drain retains the destination rather than being rounded up to success.
template <typename RetireOlderProducers>
inline MemexportDestinationPreparationOutcome
PrepareOrderedMemexportDestinations(
    const std::vector<MemexportDestinationPreparation>& preparations,
    const std::vector<PendingProducer>& producers,
    uint64_t completed_submission, MemexportPreparationTarget& target,
    RetireOlderProducers&& retire_older, bool gpu_ordered = false) {
  MemexportDestinationPreparationOutcome outcome;
  outcome.range_count = static_cast<uint32_t>(preparations.size());
  if (preparations.empty()) {
    // No destination was ever evaluated, so there is nothing this seam can
    // certify. The absence of ranges is not permission to export: the verdict
    // stays kNotEvaluated -- the one value that is never a licence -- rather
    // than borrowing kNoOlderProducer, which asserts the stronger fact that a
    // real destination was compared against the snapshot and found last. A
    // caller reaching here produced no ranges (or normalized none of them
    // successfully) and must fail its draw closed exactly as it does for an
    // unordered destination.
    return outcome;
  }
  outcome.decision = DecideMemexportDestinationOrdering(
      preparations, producers, completed_submission, gpu_ordered);
  if (outcome.decision.selected.empty()) {
    outcome.ordering = MemexportDestinationOrdering::kNoOlderProducer;
  } else if (outcome.decision.action ==
             AuthorityAction::kOrderAgainstGpuProducers) {
    // GPU order carries the chronology; the older records stay pending and
    // retire through their ordinary RAM publication.
    outcome.ordering = MemexportDestinationOrdering::kOlderProducersGpuOrdered;
  } else {
    outcome.drain = retire_older(outcome.decision);
    outcome.ordering =
        ConsumerPublicationComplete(outcome.decision, outcome.drain)
            ? MemexportDestinationOrdering::kOlderProducersRetired
            : MemexportDestinationOrdering::kOlderProducersRetained;
  }
  if (!outcome.ordered()) {
    // Deliberately before the first RequestRange: a destination whose
    // predecessors are still pending gets no pages, so a caller that ignored
    // the verdict would still find nothing uploaded to export into.
    return outcome;
  }
  // Requests are issued only now, after every older overlapping producer has
  // published its bytes to RAM, so the upload reads the converged content.
  outcome.requested = true;
  for (const MemexportDestinationPreparation& preparation : preparations) {
    if (PrepareMemexportDestination(preparation, target)) {
      ++outcome.requested_ranges;
    } else {
      outcome.requested = false;
    }
  }
  return outcome;
}

// Marks a memexport range GPU-written.
struct MemexportPublicationTarget {
  virtual ~MemexportPublicationTarget() = default;
  virtual bool CanMarkRangeWrittenByGpu() const = 0;
  virtual void MarkRangeWrittenByGpu(uint32_t base, uint32_t length) = 0;
};

// One normalized memexport GPU-write publication decision, owned by the
// exporting submission.
struct MemexportPublication {
  GuestRange range;
  uint64_t submission;
};

inline std::optional<MemexportPublication> DecideMemexportPublication(
    uint32_t base, uint32_t length, uint64_t submission) {
  std::optional<GuestRange> range = GuestRange::FromBaseLength(base, length);
  if (!range) {
    return std::nullopt;
  }
  return MemexportPublication{*range, submission};
}

// Factual result of the ordered ownership + GPU-write publication.
struct MemexportPublicationResult {
  bool registered = false;
  bool marked = false;
  AuthorityAction action = AuthorityAction::kNoDependency;
};

template <typename RegisterPending>
inline MemexportPublicationResult PublishMemexportWrite(
    const MemexportPublication& publication,
    MemexportPublicationTarget& target, RegisterPending&& register_pending) {
  MemexportPublicationResult result;
  if (!target.CanMarkRangeWrittenByGpu()) {
    return result;
  }
  // Ownership is deliberately installed BEFORE the GPU-written notification.
  // If record allocation/registration fails, the marker is never called.
  result.registered = register_pending(publication);
  if (!result.registered) {
    return result;
  }
  target.MarkRangeWrittenByGpu(publication.range.start(),
                               publication.range.length());
  result.marked = true;
  result.action = AuthorityAction::kPublishGpuWrite;
  return result;
}

// Copies one completed pending memexport record's exact bytes from the
// shared buffer to guest RAM. The port MUST NOT invalidate or overwrite the
// shared buffer -- it is the authoritative source of the converged bytes --
// and MUST report false (rather than partially converge) when the RAM
// mapping or shared-buffer contents pointer is unavailable, so the caller
// retains the pending record for retry instead of losing GPU-newer bytes.
struct MemexportConvergenceTarget {
  virtual ~MemexportConvergenceTarget() = default;
  // `bytes_copied` reports what the implementation PHYSICALLY moved, which is
  // not derivable from the range: a backing where the shared buffer aliases
  // guest RAM publishes the same bytes while moving none. It is an out
  // parameter rather than a caller-side inference precisely so no consumer of
  // this seam can compute "we must have copied length bytes" from a mode.
  virtual bool CopyBufferToRam(uint32_t base, uint32_t length,
                               uint32_t* invalidated_entries,
                               uint64_t* bytes_copied) = 0;
};

// Factual result of one convergence attempt. `converged` gates record
// retirement: the caller must retain the pending record for retry whenever
// this is false, and increment drain counters only when it is true.
struct MemexportConvergenceResult {
  bool converged = false;
  AuthorityAction action = AuthorityAction::kNoDependency;
  uint32_t invalidated_entries = 0;
  // Bytes the target actually copied. Zero is a legitimate success on an
  // aliased backing; it is never evidence that nothing was published.
  uint64_t bytes_copied = 0;
};

inline MemexportConvergenceResult ConvergeMemexportRam(
    const PendingProducer& producer, MemexportConvergenceTarget& target) {
  MemexportConvergenceResult result;
  result.converged = target.CopyBufferToRam(
      producer.range.start(), producer.range.length(),
      &result.invalidated_entries, &result.bytes_copied);
  result.action = result.converged ? AuthorityAction::kConvergeRamFromBuffer
                                   : AuthorityAction::kNoDependency;
  return result;
}

// Whether the CALLER has independently proven that a record carrying no owning
// submission tag was nevertheless made final by a synchronous GPU boundary.
//
// This vocabulary exists because an untagged record's finality comes from the
// synchronous commit-and-wait the consumer path performs, which the record
// itself cannot describe.
//
// It was written when untagged was the DEFAULT. The async promotion
// (2026-08-09) inverted that: S7LifetimeOff() is false unless XE_METAL_ASYNC=0
// or an explicit S7 kill switch is set, S7TagsOff() includes it, and the
// publication seam registers `S7TagsOff() ? 0 : CurrentEncodingSubmission()` --
// so on a normal launch every pending memexport carries a REAL submission id.
// Untagged records remain reachable (the XE_METAL_ASYNC=0 rollback, the S7
// kill switches, and anything registered before a submission exists), which is
// why this proof is still required rather than retired.
//
// The proof is therefore supplied from outside rather than inferred here, and
// ProducerComplete is deliberately left alone: teaching the generic tagged
// predicate to read 0 as completed would make every untagged record
// unconditionally publishable, including one whose boundary never ran. Passing
// the evidence as a value means the convergence site cannot fabricate it
// locally -- only a caller that actually executed the boundary can hand it in.
enum class MemexportBoundaryEvidence : uint32_t {
  // Nothing beyond the record's own submission tag. An untagged record is
  // unproven under this value and is not publishable.
  kNone,
  // The caller committed the frame command buffer, waited, and proved the
  // required submission is at or below the acquired completed high-water mark.
  kSynchronousBoundaryCompleted,
  // Not a value; one past the last enumerator. Every real enumerator must be
  // declared ABOVE this line so the count below follows the enum automatically.
  kCount,
};
inline constexpr uint32_t kMemexportBoundaryEvidenceCount =
    static_cast<uint32_t>(MemexportBoundaryEvidence::kCount);

inline constexpr std::array kMemexportBoundaryEvidences = {
    MemexportBoundaryEvidence::kNone,
    MemexportBoundaryEvidence::kSynchronousBoundaryCompleted,
};
static_assert(kMemexportBoundaryEvidences.size() ==
                  kMemexportBoundaryEvidenceCount,
              "kMemexportBoundaryEvidences must enumerate every real "
              "MemexportBoundaryEvidence value exactly once");

constexpr bool IsMemexportBoundaryEvidenceValue(
    MemexportBoundaryEvidence value) {
  return static_cast<uint32_t>(value) < kMemexportBoundaryEvidenceCount;
}

constexpr const char* MemexportBoundaryEvidenceName(
    MemexportBoundaryEvidence value) {
  switch (value) {
    case MemexportBoundaryEvidence::kNone:
      return "none";
    case MemexportBoundaryEvidence::kSynchronousBoundaryCompleted:
      return "synchronous_boundary_completed";
    case MemexportBoundaryEvidence::kCount:
      break;  // not a value
  }
  return "unknown";
}

// Why a record's convergence was or was not attempted, naming BOTH the reason
// and which of the two valid completion proofs licensed it. Two attempt values
// rather than one because "the record's own submission completed" and "the
// caller crossed a synchronous boundary on its behalf" are different claims
// with different failure modes, and a report that collapsed them could not say
// which one a run actually relied on.
enum class MemexportConvergenceGate : uint32_t {
  // Tagged (submission != 0) but the owning submission has not completed on the
  // GPU. The target is NOT called.
  kNotAttemptedIncompleteSubmission,
  // Untagged (submission 0 -- the rollback/pre-submission shape since the async
  // promotion, not the default one) and the caller supplied no
  // synchronous-boundary proof. The target is NOT called: nothing here can tell
  // a finished export from one whose commit never happened.
  kNotAttemptedUntaggedWithoutBoundary,
  // Tagged and complete against the high-water mark.
  kAttemptedCompletedSubmission,
  // Untagged, but the caller proved a completed synchronous boundary.
  kAttemptedSynchronousBoundary,
  // Not a value; one past the last enumerator. Every real enumerator must be
  // declared ABOVE this line so the count below follows the enum automatically.
  kCount,
};
inline constexpr uint32_t kMemexportConvergenceGateCount =
    static_cast<uint32_t>(MemexportConvergenceGate::kCount);

// Every REAL gate value, in declaration order. The size is DEDUCED from the
// initializer (class template argument deduction), so nothing here is a
// hand-maintained count that could drift: adding an enumerator without adding
// it to this list fails the static_assert below, and the conformance cases
// pinned to this array then have to name and classify the new value. That is
// what makes "the vocabulary is complete" a checked claim rather than a comment.
inline constexpr std::array kMemexportConvergenceGates = {
    MemexportConvergenceGate::kNotAttemptedIncompleteSubmission,
    MemexportConvergenceGate::kNotAttemptedUntaggedWithoutBoundary,
    MemexportConvergenceGate::kAttemptedCompletedSubmission,
    MemexportConvergenceGate::kAttemptedSynchronousBoundary,
};
static_assert(kMemexportConvergenceGates.size() ==
                  kMemexportConvergenceGateCount,
              "kMemexportConvergenceGates must enumerate every real "
              "MemexportConvergenceGate value exactly once");

// kCount is a bound, never an observation. Anything at or past it is a value
// the vocabulary does not define, and no derivation may treat it as one.
constexpr bool IsMemexportConvergenceGateValue(MemexportConvergenceGate value) {
  return static_cast<uint32_t>(value) < kMemexportConvergenceGateCount;
}

constexpr const char* MemexportConvergenceGateName(
    MemexportConvergenceGate value) {
  switch (value) {
    case MemexportConvergenceGate::kNotAttemptedIncompleteSubmission:
      return "not_attempted_incomplete_submission";
    case MemexportConvergenceGate::kNotAttemptedUntaggedWithoutBoundary:
      return "not_attempted_untagged_without_boundary";
    case MemexportConvergenceGate::kAttemptedCompletedSubmission:
      return "attempted_completed_submission";
    case MemexportConvergenceGate::kAttemptedSynchronousBoundary:
      return "attempted_synchronous_boundary";
    case MemexportConvergenceGate::kCount:
      break;  // not a value
  }
  return "unknown";
}

// The one place that decides whether a gate value means the target was called.
// Exhaustive, with NO `default:`, so a new enumerator is a compiler warning
// here rather than silently inheriting someone's notion of "attempted"; kCount
// and any out-of-range value fall through to false, because a value the
// vocabulary does not define can never license a publication.
constexpr bool MemexportConvergenceGateAttempts(MemexportConvergenceGate value) {
  switch (value) {
    case MemexportConvergenceGate::kNotAttemptedIncompleteSubmission:
    case MemexportConvergenceGate::kNotAttemptedUntaggedWithoutBoundary:
      return false;
    case MemexportConvergenceGate::kAttemptedCompletedSubmission:
    case MemexportConvergenceGate::kAttemptedSynchronousBoundary:
      return true;
    case MemexportConvergenceGate::kCount:
      break;  // not a value
  }
  return false;
}

// The whole gate decision as a pure value transform, so the rule can be
// enumerated exhaustively instead of being reachable only through a device.
//
// The two halves are kept strictly apart. A TAGGED record is decided by its own
// tag alone: boundary evidence is not consulted, so no caller can talk a
// genuinely unfinished submission past its own high-water mark. An UNTAGGED
// record has no tag to decide with, so it is decided by the caller's evidence
// alone.
constexpr MemexportConvergenceGate DecideMemexportConvergenceGate(
    const PendingProducer& producer, uint64_t completed_submission,
    MemexportBoundaryEvidence boundary_evidence) {
  if (producer.submission != 0) {
    return ProducerComplete(producer, completed_submission)
               ? MemexportConvergenceGate::kAttemptedCompletedSubmission
               : MemexportConvergenceGate::kNotAttemptedIncompleteSubmission;
  }
  return boundary_evidence ==
                 MemexportBoundaryEvidence::kSynchronousBoundaryCompleted
             ? MemexportConvergenceGate::kAttemptedSynchronousBoundary
             : MemexportConvergenceGate::kNotAttemptedUntaggedWithoutBoundary;
}

// The gate + the attempt as ONE value. `result` is meaningful only when
// `attempted()`; on a non-attempt it is the default (not converged, zero
// bytes), which is exactly what a caller must fall back to.
struct GatedMemexportConvergence {
  // Defaults to a NON-attempt so a default-constructed value can never be read
  // as a licensed publication.
  MemexportConvergenceGate gate =
      MemexportConvergenceGate::kNotAttemptedUntaggedWithoutBoundary;
  MemexportConvergenceResult result;

  bool attempted() const { return MemexportConvergenceGateAttempts(gate); }
  // The one predicate that licenses retiring the pending record. Both halves
  // are required: a non-attempt publishes nothing, and an attempted-but-failed
  // convergence must be retried.
  bool retires() const { return attempted() && result.converged; }
};

// Default pre-attempt observation: nothing. Named rather than an empty lambda
// so the convenience overloads visibly mean "no observation" instead of looking
// like a forgotten argument.
struct NoMemexportPreAttempt {
  void operator()() const {}
};

// The single completion gate for memexport RAM convergence, shared by the
// production command-processor drain and conformance. It exists as one
// device-free function precisely so the rule "an unproven GPU write is not
// publishable" cannot drift between the code that runs and the code that is
// tested: a test-only replica would prove only that the replica holds.
//
// Aliasing changes nothing here. A shared buffer that IS guest RAM still holds
// whatever the shader has written SO FAR; the completion boundary is what makes
// those bytes final, and skipping it because "no copy is needed" would publish
// a partially written export.
//
// `before_convergence` runs EXACTLY ONCE, after a completion proof is
// established and immediately before the target publishes. It is the only
// licensed place to read the GPU-written bytes: any sampling ahead of the gate
// would be reading an export that may still be in flight, and any sampling
// after the target would be reading post-publication bytes while claiming to
// describe the pre-publication state.
template <typename PreAttemptFn>
inline GatedMemexportConvergence ConvergeCompletedMemexportRam(
    const PendingProducer& producer, uint64_t completed_submission,
    MemexportBoundaryEvidence boundary_evidence,
    MemexportConvergenceTarget& target, PreAttemptFn&& before_convergence) {
  GatedMemexportConvergence gated;
  gated.gate = DecideMemexportConvergenceGate(producer, completed_submission,
                                              boundary_evidence);
  if (!gated.attempted()) {
    // Deliberately before BOTH the observation and any target call: an unproven
    // producer must not reach the convergence port, and its bytes must not be
    // read even diagnostically. A target that counts calls, and an observer
    // that counts invocations, therefore prove the gate directly rather than
    // inferring it from a result.
    return gated;
  }
  before_convergence();
  gated.result = ConvergeMemexportRam(producer, target);
  return gated;
}

inline GatedMemexportConvergence ConvergeCompletedMemexportRam(
    const PendingProducer& producer, uint64_t completed_submission,
    MemexportBoundaryEvidence boundary_evidence,
    MemexportConvergenceTarget& target) {
  return ConvergeCompletedMemexportRam(producer, completed_submission,
                                       boundary_evidence, target,
                                       NoMemexportPreAttempt{});
}

// Three-argument convenience. It supplies kNone, which is the SAFE default: a
// caller that has proven no boundary cannot accidentally publish an untagged
// record through it.
inline GatedMemexportConvergence ConvergeCompletedMemexportRam(
    const PendingProducer& producer, uint64_t completed_submission,
    MemexportConvergenceTarget& target) {
  return ConvergeCompletedMemexportRam(producer, completed_submission,
                                       MemexportBoundaryEvidence::kNone, target,
                                       NoMemexportPreAttempt{});
}

// One normalized RAM-writer publication decision. It is a transient value, not
// persistent authority. The factory rejects empty/out-of-domain input and
// clamps the end before any backend operation sees the range.
struct RamWritePublication {
  WriterKind writer;
  GuestRange range;
  AuthorityAction action;
  RamWritePublicationPolicy policy;
};

inline std::optional<RamWritePublication> DecideRamWritePublication(
    WriterKind writer, uint32_t base, uint32_t length) {
  std::optional<GuestRange> range = GuestRange::FromBaseLength(base, length);
  if (!range) {
    return std::nullopt;
  }
  return RamWritePublication{writer, *range, DecideWriterPublication(writer),
                             RamWritePolicyFor(writer)};
}

// Execute a normalized decision through existing backend operations. Guest CPU
// decisions intentionally execute nothing because the physical callback has
// already published them. This function owns no state and adds no
// synchronization.
inline RamWritePublicationOutcome PublishRamWrite(
    const RamWritePublication& publication,
    RamWritePublicationTarget& target) {
  RamWritePublicationOutcome outcome;
  bool required_operations_executed = true;
  if (publication.policy.execute_shared_invalidation) {
    outcome.shared_invalidation_executed = target.InvalidateSharedPages(
        publication.range.start(), publication.range.length(),
        publication.policy.exact_range);
    required_operations_executed &= outcome.shared_invalidation_executed;
  }
  if (publication.policy.execute_legacy_invalidation) {
    outcome.legacy_invalidation_executed = target.InvalidateLegacyTextures(
        publication.range.start(), publication.range.length(),
        &outcome.invalidated_entries);
    required_operations_executed &= outcome.legacy_invalidation_executed;
  }
  outcome.action = required_operations_executed
                       ? publication.action
                       : AuthorityAction::kNoDependency;
  return outcome;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_MEMORY_AUTHORITY_H_
