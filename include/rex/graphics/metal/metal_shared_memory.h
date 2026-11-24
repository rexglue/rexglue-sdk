/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_
#define XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rex/graphics/metal/metal_flat_index_map.h"
#include "rex/graphics/metal/metal_zero_copy_contract.h"
#include "rex/graphics/metal/metal_zero_copy_overlap.h"
#include "gpu/xenos/shared_memory.h"  // upstream base, written against the current memory model

namespace rex {
namespace graphics {
class TraceWriter;
namespace metal {

// Factual result of one shared-buffer -> guest-RAM convergence performed by the
// shared memory itself. Every field is recorded at the physical operation, not
// derived from the backing mode: `bytes_copied` is assigned at the memcpy that
// moved them, and `alias_skipped` is set only where the destination was PROVEN
// to be the source. A caller can therefore report what happened instead of
// restating what it configured.
struct SharedBufferConvergenceOutcome {
  // Gates pending-record retirement. False means nothing was published and the
  // caller must retain its record for retry.
  bool converged = false;
  // Bytes this call actually moved. Zero on the proven alias path.
  uint64_t bytes_copied = 0;
  // The copy was suppressed because destination and source are one allocation.
  bool alias_skipped = false;
};

// Real rex::graphics::SharedMemory subclass over a single 512 MiB shared-storage
// MTLBuffer where buffer offset == guest physical byte address, so the
// translated vertex shader's vfetch reads straight out of it. The base class
// provides page-granular validity tracking with CPU write-watch invalidation:
// uploads happen only for pages actually written since their last use. (The
// bring-up shim this replaces memcmp'd entire fetch ranges per draw — the
// terrain mega-streams are tens of MB, which became tens of GB of comparison
// per frame once real gameplay rendered.) The buffer is CPU-visible, so
// UploadRanges is a plain memcpy — no staging, no GPU commands.
//
// Zero-copy pre-gate (default off): when XE_METAL_ZERO_COPY_SHARED_MEMORY is
// exactly "1" the buffer may instead WRAP the guest physical view itself
// (newBufferWithBytesNoCopy), so buffer bytes and guest RAM are one allocation.
// Every other value, and every failed contract check, takes the mirror path
// above unchanged. See metal_zero_copy_contract.h. In that mode the upload and
// memexport-convergence copies become no-ops over bytes that are already in
// place; nothing else about validity, tracing, hazard flushing, writer
// ordering, or record retirement changes.
class MetalSharedMemory : public ::rex::gpu::SharedMemory {
 public:

  MetalSharedMemory(::rex::IPhysicalMemory& memory, ::rex::IPhysicalWriteWatch& watch, void* device);
  ~MetalSharedMemory() override;

  // TODO(migration): the old base exposed "was this page written by the GPU" directly; the new base keeps that state
  // private and offers only InvalidateAllPages()/SetSystemPageFlagsValidWithGPUDataWritten(). Answering false here is
  // the conservative choice (treat pages as not GPU-written, so they are uploaded), but it must be re-derived from the
  // new watch/validity API before this backend is trusted - it changes upload behaviour, not just compilation.
  bool IsPageGpuWrittenByBackend(uint32_t page) const {
    (void)page;
    return false;
  }

  bool Initialize();
  void Shutdown();

  // Frame-trace recording: every upload's source range is written into the
  // open trace (uploads read guest RAM, which is exactly the data a replay
  // must restore). WriteMemoryRead self-guards when no trace is open.
  void SetTraceWriter(TraceWriter* trace_writer) {
    trace_writer_ = trace_writer;
  }
  // Frame-trace init: invalidates every non-GPU-written page (so ranges the
  // traced frame consumes re-upload and get recorded) and writes collected
  // GPU-written ranges straight from guest RAM (CPU-authoritative here).
  void WriteTraceInitialState(TraceWriter& trace_writer);

  // id<MTLBuffer> the GPU binds, not owned by the caller: the shared-storage
  // mirror, or the Private (VRAM) copy under the discrete mirror below.
  void* GetBuffer() const { return gpu_buffer_ ? gpu_buffer_ : buffer_; }
  // id<MTLBuffer> whose contents the CPU reads and writes (the shared-storage
  // mirror). The same buffer as GetBuffer() unless the discrete mirror is on.
  void* GetCpuBuffer() const { return buffer_; }

  // The guest's bytes at a physical address, through the shared buffer this class owns. Upstream's memory interface hands out
  // spans rather than host pointers, and every translation in this backend goes through this buffer anyway.
  template <typename T = const void*>
  T TranslateGuestAddress(uint32_t address) const {
    return reinterpret_cast<T>(const_cast<uint8_t*>(buffer_contents_) + address);
  }

  // Discrete-GPU mirror (::rex::graphics::metal::GetMetalSettings().metal_discrete_vram_mirror.Get(), devices without
  // unified memory): the GPU binds a Private copy in VRAM instead of reading
  // the shared-storage mirror in system RAM across PCIe (Mac Pro RX 6900 XT
  // 2026-10-19: RDR 22.6 -> 28, Gears 3 23.3 -> 30, USF4 45 -> 51). Default
  // on; Apple GPUs never take it. Every CPU write into the CPU
  // mirror goes through NoteCpuWrite, whose callback encodes the CPU->VRAM
  // blit ahead of the consumers; the command processor blits GPU-produced
  // ranges back into the CPU mirror at commit and marks them consumed by
  // their submission (NoteGpuWriteInFlight), so an upload over them while the
  // readback is in flight takes the staged-blit hazard path.
  static bool DiscreteMirrorEnabled();
  // Unified-memory mirror in the default (cached) CPU mode rather than
  // write-combined, since resolve publication and memexport drains read it
  // on the CPU. Default on; XE_METAL_SHARED_MIRROR_CACHED=0 rolls back.
  static bool SharedMirrorCachedEnabled();
  bool is_discrete_mirror() const { return gpu_buffer_ != nullptr; }
  void SetCpuWriteCallback(
      std::function<void(uint32_t offset, uint32_t length)> callback) {
    cpu_write_callback_ = std::move(callback);
  }
  // Call BEFORE the memcpy into the CPU mirror: it also drops the range from
  // the VRAM-newer table, under the lock an on-demand fetch copies under, so
  // a fetch can never land older VRAM bytes over the CPU's newer ones.
  void NoteCpuWrite(uint32_t offset, uint32_t length);

  // On-demand readback (discrete mirror). GPU-produced ranges the CPU rarely
  // reads (lazily published resolves) are not blitted back every frame; they
  // are recorded here as "VRAM newer than the CPU mirror" with their
  // producing submission. A CPU reader of the mirror calls EnsureCpuCurrent
  // first: it waits for the newest committed producer overlapping the range,
  // blits just those bytes to a staging buffer on a separate queue (so it
  // does not wait behind the frames queued on the main one), and copies the
  // still-VRAM-newer bytes into the mirror. A range fetched once is "hot":
  // the command processor reads its later productions back with the frame.
  // `eager` entries are frame readbacks, kept only so a fetch also waits for
  // them; they leave when their submission completes. Any thread.
  static bool OnDemandReadbackEnabled();
  void SetVramFetchHooks(std::function<uint64_t()> encoding_submission,
                         std::function<uint64_t()> completed_submission,
                         std::function<bool(uint64_t)> wait_for_submission) {
    vram_encoding_submission_ = std::move(encoding_submission);
    vram_completed_submission_ = std::move(completed_submission);
    vram_wait_ = std::move(wait_for_submission);
  }
  void MarkVramNewer(uint32_t start, uint32_t length, uint64_t submission,
                     bool eager);
  bool IsVramHot(uint32_t origin) const;
  // False only when a producer could not be waited for or the fetch failed;
  // the mirror then still holds what it held.
  bool EnsureCpuCurrent(uint32_t start, uint32_t length);
  struct VramFetchStats {
    uint64_t fetches = 0;
    uint64_t bytes = 0;
    uint64_t ns = 0;
    size_t entries = 0;
    size_t hot = 0;
  };
  VramFetchStats ConsumeVramFetchStats();
  void NoteGpuWriteInFlight(uint32_t start, uint32_t length,
                            uint64_t submission);

  // Whether the installed buffer aliases guest RAM rather than mirroring it.
  // Decided once in Initialize and never transitioned afterwards: consumers
  // must not have to ask whether the backing changed mid-run. False until a
  // successful Initialize and again after Shutdown.
  bool is_zero_copy() const { return is_zero_copy_; }
  // The facts the single startup line was derived from, kept for conformance
  // and for the pre-gate's control arms (an exact-off run must show a
  // completely untouched observation).
  const ZeroCopyObservation& zero_copy_observation() const {
    return zero_copy_observation_;
  }

  // Frame-batching correctness: every draw in the open frame command buffer
  // reads the buffer's state at COMMIT, not at encode. When a re-upload is
  // about to land in pages a draw encoded this frame has already consumed
  // (the intra-frame dynamic rewrite: UI glyph rings, particles), the flush
  // callback (commit + wait, set by the command processor) runs FIRST so the
  // encoded draws read the old data.
  void SetHazardFlushCallback(std::function<void()> callback) {
    hazard_flush_callback_ = std::move(callback);
  }
  // Staged (no-wait) hazard upload, stage 3 of
  // optimize-metal-gpu-resident-resolve-coherence: instead of committing and
  // WAITING for the frame so the memcpy can land, the open frame command
  // buffer is committed without a wait (`commit`) and the new bytes travel as
  // a blit encoded into the next command buffer (`stage`), which executes
  // after the in-flight readers in queue order. `stage` copies
  // [offset, offset+length) of guest RAM into a staging buffer, encodes the
  // blit into the shared buffer and returns the submission it was encoded
  // into (0 = could not stage; the caller falls back to commit + wait). The
  // staged range is then recorded as consumed by that submission so a later
  // direct memcpy over it is itself detected as a hazard until the blit has
  // executed.
  // Vertex-fetch rename (default on; XE_METAL_VFETCH_RENAME=0 rollback): when a vertex-fetch
  // upload would hazard against draws still queued in the open command
  // buffer, the fetched range is copied from guest RAM into a rename arena at
  // the tail of the mirror buffer and the draw's fetch constant is pointed at
  // it, so the queued draws keep their bytes, this draw gets the new ones,
  // and no transfer encoder (= no render-pass end) is needed. The mirror's
  // pages stay INVALID for the range: any later consumer re-requests it
  // through the ordinary upload path (a texture or resolve consumer then
  // takes the staged route; a draw renames again). LEGO Star Wars II: 21 of
  // its 81 render passes per frame ended for staged hazard uploads
  // (2026-10-04). Mirror mode only; zero-copy has no arena.
  static constexpr uint32_t kRenameArenaBytes = 16u * 1024u * 1024u;
  struct VfetchRename {
    uint32_t guest_start = 0;
    uint32_t length = 0;
    uint32_t buffer_offset = 0;  // byte offset in the shared buffer (>= kBufferSize)
  };
  static bool VfetchRenameEnabled();
  // RequestConsumedRange for a draw's vertex fetch: on a hazard the range is
  // renamed (rename_out->length != 0) instead of uploaded, and is then NOT a
  // consumed range of the open buffer (the arena is never rewritten while its
  // submission is in flight).
  bool RequestVertexFetchRange(uint32_t start, uint32_t length,
                               VfetchRename* rename_out);
  // Length the GPU may address through the shared-memory descriptors
  // (kBufferSize, plus the rename arena when it exists).
  uint32_t gpu_visible_length() const { return gpu_visible_length_; }
  void SetCompletedSubmissionProvider(std::function<uint64_t()> provider) {
    completed_submission_provider_ = std::move(provider);
  }
  uint64_t vfetch_renames() const { return vfetch_renames_; }
  uint64_t vfetch_rename_bytes() const { return vfetch_rename_bytes_; }
  uint64_t vfetch_rename_refusals() const { return vfetch_rename_refusals_; }

  void SetHazardCommitCallback(std::function<void()> callback) {
    hazard_commit_callback_ = std::move(callback);
  }
  void SetStagedUploadCallback(
      std::function<uint64_t(uint32_t offset, uint32_t length)> callback) {
    staged_upload_callback_ = std::move(callback);
  }
  // GPU-current span provider (straddler fix, stage 4 of
  // optimize-metal-gpu-resident-resolve-coherence): validity is tracked per
  // host page (16 KiB on Apple Silicon), so a CPU write to a neighbour in
  // the same page invalidates the page and the next upload would copy stale
  // RAM over a GPU-resident resolve / memexport fragment sharing it (SF4
  // 10395: the two luminance-pyramid resolves). The provider appends the
  // byte-exact spans of every pending GPU-current record overlapping
  // [start, start + length); UploadRanges copies around them.
  using GpuCurrentSpanProvider = std::function<void(
      uint32_t start, uint32_t length,
      std::vector<std::pair<uint32_t, uint32_t>>* out)>;
  void SetGpuCurrentSpanProvider(GpuCurrentSpanProvider provider) {
    gpu_current_span_provider_ = std::move(provider);
  }
  // Frame-trace recording: called with every upload's source range before it
  // is written into the open trace, so the owner can first publish pending
  // GPU-current bytes to guest RAM. Without it a trace records RAM that is
  // stale behind an unpublished resolve (Rise of the Tomb Raider's colour
  // LUT slices), and a replay restores those stale bytes over the GPU's.
  void SetTracePublishCallback(std::function<void(uint32_t, uint32_t)> cb) {
    trace_publish_callback_ = std::move(cb);
  }

  // RequestRange plus recording [start, start+length) as consumed by the
  // current frame for the hazard detection above.
  bool RequestConsumedRange(uint32_t start, uint32_t length);

  // After a GPU-current publication the walked bytes are IDENTICAL in the
  // buffer and RAM (the publication just copied buffer -> RAM, and the
  // extent's padding was committed from RAM at dispatch). Mark the pages
  // valid-not-gpu-written so consumers stop re-uploading bytes the buffer
  // already holds (the 4.2 probes measured +45% tex uploads/frame from the
  // publication's blanket invalidation). Watches for the content CHANGE were
  // already fired by the publication's invalidate; this only restores page
  // validity.
  void MarkRangeRamEqual(uint32_t start, uint32_t length) {
    MarkRangeValid(start, length, /*written_by_gpu=*/false);
  }

  // Restore valid+gpu_written bits WITHOUT firing watches. For the straddler
  // re-assert only: the range's watches already fired at the original
  // RangeWrittenByGpu when the record was created, and the content has not
  // changed since — but RangeWrittenByGpu's FireWatches on every drain tore
  // down resolved-target bridges each frame (probe #4's 10.9x slot churn).
  void ReassertGpuWritten(uint32_t start, uint32_t length) {
    MarkRangeValid(start, length, /*written_by_gpu=*/true);
  }

  // True when any system page of [start, start + length) holds GPU-written
  // content the buffer has that RAM does not (valid + gpu_written). A CPU
  // read of RAM over such a range is stale by definition; the shared texture
  // cache's hash revalidation must reload instead of comparing. CP thread
  // only, like every other validity user.
  bool AnyPageGpuWritten(uint32_t start, uint32_t length) const {
    if (!length || start >= kBufferSize) {
      return false;
    }
    const uint32_t shift = GetPageSizeLog2();
    const uint32_t last = std::min<uint32_t>(uint64_t(start) + length - 1,
                                             kBufferSize - 1);
    for (uint32_t page = start >> shift; page <= (last >> shift); ++page) {
      if (IsPageGpuWrittenByBackend(page)) {
        return true;
      }
    }
    return false;
  }

  // True when a pending GPU producer (an unpublished resolve writeback or a
  // memexport, GPU-current in the buffer) overlaps [start, start + length):
  // its bytes land when its command buffer runs, so a load committed ahead
  // of the frame buffer must not read there. CP thread only.
  bool AnyPendingGpuProducerSpan(uint32_t start, uint32_t length) {
    if (!length || !gpu_current_span_provider_) {
      return false;
    }
    producer_span_scratch_.clear();
    gpu_current_span_provider_(start, length, &producer_span_scratch_);
    return !producer_span_scratch_.empty();
  }

  // True when a staged hazard upload (a blit encoded into the open frame
  // command buffer, see UploadRanges) still owes bytes to any part of
  // [start, start + length) for `open_submission`: the buffer holds the OLD
  // bytes until that command buffer runs, so a load committed ahead of it (a
  // side command buffer) would read them. CP thread only.
  bool AnyStagedUploadPending(uint32_t start, uint32_t length,
                              uint64_t open_submission) const {
    if (!length || staged_open_.empty()) {
      return false;
    }
    const uint64_t end = uint64_t(start) + length;
    for (const StagedRange& s : staged_open_) {
      if (s.submission == open_submission && start < s.end && end > s.start) {
        return true;
      }
    }
    return false;
  }

  // Validity page size for the straddler re-assert's fragment math (the base
  // accessor is protected).
  uint32_t validity_GetPageSizeLog2() const { return GetPageSizeLog2(); }

  // Public forwarder of the base per-range watch fire (no global fan-out, no
  // validity change) — the content-change signal for watching textures when
  // a GPU-current producer registers; see the base declaration.
  // TODO(migration): the old base had FireRangeWatchesOnly(start, length, invalidated_by_gpu); the new one exposes
  // RangeWrittenByGPU(start, length), which fires the same watches without the invalidated_by_gpu hint. The hint is
  // dropped here deliberately rather than guessed at.
  void FireRangeWatchesBackend(uint32_t start, uint32_t length, bool invalidated_by_gpu) {
    (void)invalidated_by_gpu;
    RangeWrittenByGPU(start, length);
  }

  // S8: the submission the currently-open command buffer will get — consumed
  // ranges are tagged with it so ReleaseConsumedRanges frees exactly the
  // ranges whose reader has completed (a blanket clear would drop hazard
  // protection for LATER in-flight submissions).
  void SetSubmissionProvider(std::function<uint64_t()> provider) {
    submission_provider_ = std::move(provider);
  }
  void ReleaseConsumedRanges(uint64_t completed_submission);

  // --- Zero-copy in-flight CPU rewrite evidence (pre-gate tasks 5.1-5.4) ----
  //
  // Two providers, because the evidence needs two INDEPENDENT sources (design
  // D6 / D2a). Both are installed once, before Initialize, and are only read
  // afterwards: the faulting guest thread invokes one of them and must not race
  // an assignment.

  // EXECUTION. Deliberately NOT the S7-governed submission_provider_ above.
  // That one returns `S7TagsOff() ? 0 : CurrentEncodingSubmission()`, and
  // S7TagsOff() is true on every default launch, so it reports 0 — which is the
  // idle sentinel. Reusing it would stamp every page with "nobody is reading
  // this", and the overlap counter could then only ever be zero, for a reason
  // that has nothing to do with whether an overlap happened. This provider must
  // report the real encoding submission in every mode.
  void SetOverlapSubmissionProvider(std::function<uint64_t()> provider) {
    overlap_submission_provider_ = std::move(provider);
  }
  // COMPLETION, supplied separately from the stamps so no single source both
  // records the claim and decides whether it is still outstanding. Read once
  // per invalidation callback, on the faulting guest thread, so it must be an
  // atomic load and nothing more.
  void SetOverlapCompletedSubmissionProvider(
      std::function<uint64_t()> provider) {
    overlap_completed_submission_provider_ = std::move(provider);
  }
  // Bounded evidence for the lifecycle reporter. Inert (and reported as
  // inactive) unless alias mode installed the observer.
  //
  // Taken under the physical-invalidation callback lock, so the counters it
  // returns are one coherent moment rather than a set of independently-timed
  // reads that could, say, show more classified pages than observed callbacks.
  // Bounded and allocation-free inside the lock: a fixed-size struct copy and
  // no logging (the caller formats after it returns).
  ZeroCopyOverlapSnapshot zero_copy_overlap_snapshot() const;

  // Forgets the consumed-range set (call after the frame command buffer has
  // been committed and waited on).
  void ResetFrameRanges() {  // sync-mode/shutdown
    consumed_ranges_.clear();
    consumed_index_.Clear();
    ClearConsumedPages();
  }
  // Post-clear evidence: submission-tagged hazard records still held. At a
  // quiesced boundary every submission has completed, so ReleaseConsumedRanges
  // (run by the pumped completion actions) should already have emptied this --
  // a claim the clear verifies rather than assumes.
  uint32_t consumed_range_count() const {
    return uint32_t(consumed_ranges_.size());
  }

  // Publishes one completed memexport's exact bytes from the shared buffer to
  // guest RAM (the byte-moving half of the typed MemexportConvergenceTarget
  // port; the caller still owns completion gating, cache invalidation and
  // record retirement). It NEVER touches shared-buffer validity -- the buffer
  // is the authoritative source of the converged bytes.
  //
  // This lives here, on the shared memory that owns the backing, because it is
  // the only place that can PROVE whether destination and source are one
  // allocation. A caller deciding that from is_zero_copy() would be inferring a
  // physical fact from a mode; this returns what the operation actually did.
  // A claimed alias whose destination is not the source fails (converged
  // false), so a wrong alias verdict retains the record instead of silently
  // publishing nothing.
  SharedBufferConvergenceOutcome ConvergeRangeToRam(uint32_t base,
                                                    uint32_t length);
  // As ConvergeRangeToRam, but copies only the words that differ from
  // `pre_image` (the range's buffer bytes before a memexport ran; it is
  // indexed from `base`). bytes_copied counts the words moved.
  SharedBufferConvergenceOutcome ConvergeChangedWordsToRam(
      uint32_t base, uint32_t length, const uint8_t* pre_image);

  // Upload copy accounting, updated at the memcpy / alias-skip site itself
  // (never from the mode). Saturating rather than wrapping: an overflowed
  // counter that read as a small number would be indistinguishable from
  // "almost no copying happened", which is exactly the claim the zero-copy
  // pre-gate has to measure.
  uint64_t upload_bytes_copied() const { return upload_bytes_copied_; }
  uint64_t upload_bytes_aliased() const { return upload_bytes_aliased_; }

  // Cache-clear release (implement-metal-cache-clearing step 5): the inherited
  // clear (which fires every watch and revalidates page state, keeping
  // GPU-written data) plus the Metal-only hazard records the base cannot know
  // about.
  void ClearCache() override;

 protected:
  bool UploadRanges(const std::vector<std::pair<u32, u32>>& upload_page_ranges) override {
    return UploadRangesImpl(upload_page_ranges.data(),
                            uint32_t(upload_page_ranges.size()));
  }
  bool UploadRangesImpl(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                        uint32_t num_upload_ranges);

  // The guest-CPU write observation half of the overlap evidence. Invoked by
  // the base class's physical-invalidation THUNK only, so the backend's own
  // publications (which call MemoryInvalidationCallback directly) are not
  // counted as CPU writes. Runs on the faulting guest thread.
  void ObservePhysicalCpuWrite(uint32_t physical_address_start,
                               uint32_t length);

 private:
  TraceWriter* trace_writer_ = nullptr;
  void* device_;                        // id<MTLDevice>, not owned.
  void* buffer_ = nullptr;              // id<MTLBuffer>, retained.
  void* gpu_buffer_ = nullptr;  // id<MTLBuffer> Private, retained (discrete).
  std::function<void(uint32_t, uint32_t)> cpu_write_callback_;
  struct VramNewer {
    uint32_t start;
    uint32_t end;
    uint64_t submission;
    uint32_t origin;  // start of the produced range (the hot key)
    bool eager;
  };
  // Removes [start, end) from every entry `pred` accepts. vram_mutex_ held.
  template <typename Pred>
  void SubtractVramNewerLocked(uint32_t start, uint64_t end, Pred pred);
  void PruneVramNewerLocked();
  mutable std::mutex vram_mutex_;
  std::vector<VramNewer> vram_newer_;
  size_t vram_prune_at_ = 64;
  bool vram_overflowed_ = false;
  std::unordered_set<uint32_t> vram_hot_;
  void* fetch_queue_ = nullptr;  // id<MTLCommandQueue>, retained (discrete).
  std::function<uint64_t()> vram_encoding_submission_;
  std::function<uint64_t()> vram_completed_submission_;
  std::function<bool(uint64_t)> vram_wait_;
  VramFetchStats vram_stats_;
  uint8_t* buffer_contents_ = nullptr;  // CPU-visible pointer into buffer_.
  bool is_zero_copy_ = false;
  ZeroCopyObservation zero_copy_observation_;
  uint64_t upload_bytes_copied_ = 0;
  uint64_t upload_bytes_aliased_ = 0;

  // Byte ranges consumed by draws encoded in the open frame command buffer.
  struct ConsumedRange {
    uint32_t first;   // start
    uint32_t second;  // end
    uint64_t submission;
  };
  std::vector<ConsumedRange> consumed_ranges_;
  // Staged hazard uploads not yet executed (see AnyStagedUploadPending);
  // entries of an older submission are dropped as new ones are appended.
  struct StagedRange {
    uint32_t start;
    uint32_t end;
    uint64_t submission;
  };
  std::vector<StagedRange> staged_open_;
  std::vector<std::pair<uint32_t, uint32_t>> producer_span_scratch_;
  // Exact-key lookup (start << 32 | end -> index) for the common case of a
  // range consumed again by a later draw (the same vertex buffer): the
  // containment scan over ~1,000 live ranges per vertex fetch was 5% of
  // Red Dead's command-processor thread (2026-10-05). Rebuilt whenever the
  // vector is compacted.
  MetalFlatIndexMap consumed_index_;
  // 64 KiB page -> indices (ascending) of the consumed ranges covering it
  // (XE_METAL_CONSUMED_PAGE_INDEX=0 restores the scans). A range containing
  // [start, end) covers start's page, and a range overlapping an upload
  // covers one of its pages, so the containment lookup and the upload hazard
  // test visit only those candidates, in the same order as the scans.
  static constexpr uint32_t kConsumedPageShift = 16;
  std::vector<std::vector<uint32_t>> consumed_pages_;
  std::vector<uint32_t> consumed_pages_touched_;
  // ReleaseConsumedRanges scratch: old index -> compacted index.
  std::vector<uint32_t> consumed_remap_;
  std::vector<uint32_t> upload_candidates_;  // scratch for UploadRanges
  static bool ConsumedPageIndexEnabled();
  void IndexConsumedRange(uint32_t index);
  void ClearConsumedPages();
  void AppendConsumedRange(uint32_t start, uint32_t end, uint64_t submission);
  void RebuildConsumedIndex();
  bool NoteConsumed(uint32_t start, uint32_t length, bool result);
  bool AllocateRename(uint32_t length, uint32_t* arena_offset_out);
  struct RenameFence {
    uint32_t start, end;
    uint64_t submission;
  };
  std::deque<RenameFence> rename_fences_;
  uint32_t rename_head_ = 0;
  uint32_t gpu_visible_length_ = kBufferSize;
  bool rename_context_ = false;
  uint32_t rename_request_start_ = 0, rename_request_length_ = 0;
  bool rename_happened_ = false;
  VfetchRename rename_result_;
  std::function<uint64_t()> completed_submission_provider_;
  uint64_t vfetch_renames_ = 0, vfetch_rename_bytes_ = 0,
           vfetch_rename_refusals_ = 0;
  std::function<uint64_t()> submission_provider_;
  std::function<void()> hazard_flush_callback_;
  std::function<void()> hazard_commit_callback_;
  std::function<uint64_t(uint32_t, uint32_t)> staged_upload_callback_;
  GpuCurrentSpanProvider gpu_current_span_provider_;
  std::function<void(uint32_t, uint32_t)> trace_publish_callback_;
  uint64_t upload_bytes_skipped_gpu_current_ = 0;
  uint64_t staged_uploads_ = 0;
  uint64_t staged_upload_bytes_ = 0;
  uint64_t hazard_waits_ = 0;

  // Fixed-size (512 MiB / host page) and installed ONLY in alias mode, which is
  // what makes every other arm structurally silent rather than silent by
  // convention. Arming it is a POSTCONDITION of accepting the alias, not a
  // consequence: if it cannot be armed the alias candidate is released and the
  // mirror is installed instead, because an unwatched alias would make "no
  // overlap was reported" mean "nobody was looking".
  ZeroCopyOverlapObserver zero_copy_overlap_;
  std::function<uint64_t()> overlap_submission_provider_;
  std::function<uint64_t()> overlap_completed_submission_provider_;
};

// Result of one guest-RAM alias attempt. `buffer` is a retained id<MTLBuffer>
// (or nullptr); the caller owns it and must transfer or release it.
struct ZeroCopyAliasAttempt {
  void* buffer = nullptr;
  ZeroCopyPrecondition precondition = ZeroCopyPrecondition::kNotEvaluated;
  ZeroCopyDeviceStep device_step = ZeroCopyDeviceStep::kNotAttempted;
};

// The whole alias decision for one (device, base, length) request: device-free
// preconditions, then the single-VM-region proof, then
// newBufferWithBytesNoCopy with shared storage and Metal's DEFAULT tracked
// hazard mode, then the contents-identity postcondition. Nothing is returned
// unless every step passed; a buffer that fails the postcondition is released
// here rather than handed back.
//
// Exposed rather than kept private to Initialize because the rejection classes
// are only reachable on a real device with hostile inputs (an unaligned base,
// a length past maxBufferLength, an mprotect-split region) that the production
// initializer, by construction, never produces.
ZeroCopyAliasAttempt TryCreateZeroCopyAliasBuffer(void* device, void* base,
                                                  uint64_t length,
                                                  bool force_rejection);

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_
