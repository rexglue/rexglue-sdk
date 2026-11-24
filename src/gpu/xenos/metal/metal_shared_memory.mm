/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <unistd.h>
#include "rex/graphics/metal/metal_shared_memory.h"

#include "rex/graphics/metal/metal_lazy_publication.h"

#include <algorithm>

#include "rex/graphics/trace_writer.h"

#import <Metal/Metal.h>

#include <cstdlib>
#include <chrono>
#include <cstring>

#include <rex/graphics/metal/metal_settings.h>
#include <rex/logging/log_category.h>
#include "rex/memory.h"


namespace rex {
namespace graphics {
namespace metal {

namespace {

// Apple's newBufferWithBytesNoCopy contract requires the wrapped allocation to
// occupy ONE virtual-memory region. QueryProtect is this project's wrapper over
// mach_vm_region (see base/memory_posix.cc): it reports the region containing
// `base` and, in `length`, the distance from `base` to that region's end. So
// the whole half-open interval is inside one region exactly when that distance
// covers it. Checked before the Metal call because a wrap that spans a region
// boundary is undefined rather than merely unsupported -- Metal is under no
// obligation to fail it.
bool IsSingleVmRegion(void* base, uint64_t length) {
  size_t region_remaining = 0;
  rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
  if (!rex::memory::QueryProtect(base, region_remaining, access)) {
    return false;
  }
  return uint64_t(region_remaining) >= length;
}

// Byte counters here are diagnostics that run for the whole life of a session
// and are read as evidence. A wrapped total would read as a small number, i.e.
// as "almost nothing was copied" -- the exact claim this pre-gate measures --
// so they saturate instead.
uint64_t SaturatingAdd(uint64_t accumulated, uint64_t added) {
  return added > UINT64_MAX - accumulated ? UINT64_MAX : accumulated + added;
}

}  // namespace

ZeroCopyAliasAttempt TryCreateZeroCopyAliasBuffer(void* device_handle,
                                                  void* base, uint64_t length,
                                                  bool force_rejection) {
  ZeroCopyAliasAttempt attempt;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_handle;

  ZeroCopyContractInputs inputs;
  inputs.base_address = uint64_t(reinterpret_cast<uintptr_t>(base));
  inputs.length = length;
  inputs.host_page_size = uint64_t(sysconf(_SC_PAGESIZE));  // the host granularity this contract describes
  inputs.device_max_buffer_length = device ? uint64_t([device maxBufferLength])
                                           : 0;
  inputs.force_rejection = force_rejection;
  attempt.precondition = EvaluateZeroCopyPreconditions(inputs);
  if (attempt.precondition != ZeroCopyPrecondition::kSatisfied) {
    return attempt;
  }

  if (!IsSingleVmRegion(base, length)) {
    attempt.device_step = ZeroCopyDeviceStep::kVmRegionRejected;
    return attempt;
  }

  // Shared storage with Metal's DEFAULT hazard tracking. Untracked hazards are
  // explicitly NOT taken here: the pre-gate changes where bytes live, not who
  // orders in-flight GPU work, and the driver's tracking is what keeps the
  // existing encoder ordering honest. No write-combined cache mode either --
  // this mapping is ordinary cached guest RAM the CPU reads, and describing it
  // otherwise would be a lie about memory Metal does not own.
  id<MTLBuffer> buffer =
      [device newBufferWithBytesNoCopy:base
                                length:size_t(length)
                               options:MTLResourceStorageModeShared |
                                       MTLResourceHazardTrackingModeDefault
                           deallocator:nil];
  if (!buffer) {
    attempt.device_step = ZeroCopyDeviceStep::kAllocationRejected;
    return attempt;
  }
  // The postcondition that makes this an alias at all (see
  // ZeroCopyContentsAreAlias).
  if (!ZeroCopyContentsAreAlias([buffer contents], base)) {
    attempt.device_step = ZeroCopyDeviceStep::kContentsRejected;
    buffer = nil;  // ARC releases the partial result; nothing escapes.
    return attempt;
  }
  attempt.device_step = ZeroCopyDeviceStep::kAccepted;
  attempt.buffer = (__bridge_retained void*)buffer;
  return attempt;
}

MetalSharedMemory::MetalSharedMemory(IPhysicalMemory& memory, IPhysicalWriteWatch& watch, void* device)
    : ::rex::gpu::SharedMemory(memory, watch), device_(device) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;

  // Exact opt-in. Parsed once, here, and recorded independently of what the
  // attempt below concludes.
  zero_copy_observation_ = ZeroCopyObservation();
  zero_copy_observation_.requested =
      IsZeroCopyGateEnabled(std::getenv(kZeroCopyGateEnvVar));

  id<MTLBuffer> buffer = nil;
  if (zero_copy_observation_.requested) {
    ZeroCopyAliasAttempt attempt = TryCreateZeroCopyAliasBuffer(
        device_, TranslateGuestAddress<void*>(0), kBufferSize,
        IsZeroCopyGateEnabled(std::getenv(kZeroCopyForceRejectEnvVar)));
    zero_copy_observation_.precondition = attempt.precondition;
    zero_copy_observation_.device_step = attempt.device_step;
    if (attempt.buffer) {
      id<MTLBuffer> candidate = (__bridge_transfer id<MTLBuffer>)attempt.buffer;
      // OBSERVER READINESS IS A POSTCONDITION OF ACCEPTING THE ALIAS.
      //
      // Mirroring guest RAM gives the backend a snapshot; aliasing removes it,
      // so an alias nobody is watching cannot be distinguished from an alias
      // nobody raced. Ignoring this result would leave is_zero_copy_ true over
      // an inactive, permanently empty observer, and the overlap report treats
      // an empty snapshot as the required silence -- i.e. the run would read as
      // clean evidence precisely because no evidence was being collected.
      //
      // So a failure here REJECTS the candidate rather than degrading it: the
      // alias buffer is released, the pre-existing mirror is allocated below,
      // and the verdict is the named observer_rejected step. It is deliberately
      // not fatal -- the exact fallback this pre-gate already relies on is
      // available, and failing the whole emulator over a diagnostic that is
      // default-off would be a worse answer than not aliasing.
      //
      // The table is allocated HERE, before InitializeCommon registers the
      // physical invalidation callback, because the faulting guest thread that
      // fills it may not allocate.
      if (zero_copy_overlap_.Initialize(
              kBufferSize, GetPageSizeLog2(),
              IsZeroCopyGateEnabled(
                  std::getenv(kZeroCopyForceObserverRejectEnvVar)))) {
        buffer = candidate;
      } else {
        // Leave nothing half-armed: an inactive observer is inert, so its
        // stamping and observation entries do nothing whatever a caller asks.
        zero_copy_overlap_.Shutdown();
        candidate = nil;  // ARC releases the alias; guest RAM is untouched.
        zero_copy_observation_.device_step =
            ZeroCopyDeviceStep::kObserverRejected;
      }
    }
  }

  if (!buffer) {
    // The mirror allocation: shared storage so uploads are plain host memcpys
    // (unified memory on Apple Silicon).
    const uint32_t mirror_length =
        kBufferSize + (VfetchRenameEnabled() ? kRenameArenaBytes : 0u);
    // Discrete mirror: the CPU mirror is read back on the CPU (publication),
    // so it keeps the default cached mode; the GPU copy lives in VRAM.
    const bool discrete =
        DiscreteMirrorEnabled() && ![device hasUnifiedMemory];
    // The unified mirror is read on the CPU too: GPU-current resolve
    // publication and memexport drains copy it into guest RAM. Write-combined
    // CPU reads are uncached, about 0.5 GB/s on the M1 Max: Super Street
    // Fighter II Turbo HD Remix publishes two 4 MB 1080p bands every frame
    // (8.5 ms each, the frame at 30 fps); cached, 60 (2026-11-02).
    // XE_METAL_SHARED_MIRROR_CACHED=0 restores the write-combined mirror.
    const bool cached = discrete || SharedMirrorCachedEnabled();
    buffer = [device newBufferWithLength:mirror_length
                                 options:cached
                                             ? MTLResourceStorageModeShared
                                             : (MTLResourceStorageModeShared |
                                                MTLResourceCPUCacheModeWriteCombined)];
    gpu_visible_length_ = mirror_length;
    if (!buffer) {
      REXLOG_ERROR("MetalSharedMemory: failed to allocate {} MiB shared buffer",
             kBufferSize / (1024 * 1024));
      return false;
    }
    if (discrete) {
      id<MTLBuffer> gpu = [device newBufferWithLength:mirror_length
                                              options:MTLResourceStorageModePrivate];
      if (gpu) {
        gpu.label = @"shared memory (VRAM)";
        gpu_buffer_ = (__bridge_retained void*)gpu;
        if (OnDemandReadbackEnabled()) {
          id<MTLCommandQueue> queue = [device newCommandQueue];
          queue.label = @"shared memory fetch";
          fetch_queue_ = queue ? (__bridge_retained void*)queue : nullptr;
        }
        REXLOG_INFO("MetalSharedMemory: discrete mirror, {} MiB in VRAM",
               mirror_length / (1024 * 1024));
      } else {
        REXLOG_WARN("MetalSharedMemory: no VRAM copy; GPU reads system RAM");
      }
    }
  }
  buffer_ = (__bridge_retained void*)buffer;
  buffer_contents_ = static_cast<uint8_t*>([buffer contents]);

  // Second, independent source for the verdict: re-derived from the buffer
  // actually installed, not from the attempt record. "It aliases" means the
  // installed bytes ARE guest RAM -- a claim the mirror allocation can never
  // satisfy, whatever the attempt believed.
  zero_copy_observation_.alias_buffer_installed =
      buffer_contents_ != nullptr &&
      buffer_contents_ == TranslateGuestAddress<uint8_t*>(0);
  is_zero_copy_ = zero_copy_observation_.alias_buffer_installed;

  // The observer is armed if and only if the INSTALLED backing aliases guest
  // RAM -- re-checked here rather than assumed, because the arming above keyed
  // off the attempt while is_zero_copy_ comes from the independent
  // re-derivation. If the two disagree, the mirror is what the GPU actually
  // reads, and an armed observer over a mirror would be alias-only work inside
  // a control arm. Disarm; the startup line reports the disagreement as
  // inconsistent_observation, which is the loud half of this pair.
  if (zero_copy_overlap_.active() && !is_zero_copy_) {
    zero_copy_overlap_.Shutdown();
  }

  // One bounded line, and only when the probe was requested: an exact-off run
  // must produce no zero-copy observation at all.
  if (zero_copy_observation_.requested) {
    ZeroCopyStartupReport report =
        DeriveZeroCopyStartupReport(zero_copy_observation_);
    REXLOG_INFO(
        "MetalSharedMemory: zero-copy requested, verdict {} "
        "(precondition {}, device step {})",
        ZeroCopyStartupReportName(report),
        ZeroCopyPreconditionName(zero_copy_observation_.precondition),
        ZeroCopyDeviceStepName(zero_copy_observation_.device_step));
  }

  InitializeCommon();
  return true;
}

void MetalSharedMemory::Shutdown() {
  ShutdownCommon();
  // Strictly AFTER ShutdownCommon, which unregisters the physical invalidation
  // callback: releasing the fixed stamp table while a guest thread could still
  // enter ObservePhysicalCpuWrite would be a use-after-free. Note the evidence
  // dies here, so any final report must be taken BEFORE the shared memory is
  // destroyed (MetalCommandProcessor::ShutdownContext does).
  zero_copy_overlap_.Shutdown();
  consumed_ranges_.clear();
  consumed_index_.Clear();
  ClearConsumedPages();
  if (fetch_queue_) {
    id<MTLCommandQueue> released_queue =
        (__bridge_transfer id<MTLCommandQueue>)fetch_queue_;
    (void)released_queue;
    fetch_queue_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(vram_mutex_);
    vram_newer_.clear();
    vram_hot_.clear();
  }
  if (gpu_buffer_) {
    id<MTLBuffer> released_gpu = (__bridge_transfer id<MTLBuffer>)gpu_buffer_;
    (void)released_gpu;
    gpu_buffer_ = nullptr;
  }
  if (buffer_) {
    id<MTLBuffer> released = (__bridge_transfer id<MTLBuffer>)buffer_;
    (void)released;
    buffer_ = nullptr;
    buffer_contents_ = nullptr;
  }
  // In alias mode the released buffer never owned the bytes (deallocator nil),
  // so guest RAM outlives it untouched.
  is_zero_copy_ = false;
  zero_copy_observation_ = ZeroCopyObservation();
}

bool MetalSharedMemory::RequestConsumedRange(uint32_t start, uint32_t length) {
  if (!length) {
    return true;
  }
  // RequestRange uploads only the pages invalidated since their last use; a
  // hazard flush (see UploadRanges) may run inside.
  bool result = RequestRange(start, length);
  // Task 5.1: enrol this range's HOST pages as claimed by the submission being
  // encoded right now. Two conditions, both load-bearing:
  //   * `is_zero_copy_` -- mirror mode must not even ASK the provider for a
  //     submission, because the pre-gate's control arms prove silence by
  //     counting provider calls, not by reading a counter a mirror run would
  //     leave at zero either way.
  //   * `result` -- a failed request served no consumer, so recording one would
  //     manufacture an enrollment that never happened and make later overlap
  //     counts unfalsifiable in the loud direction.
  // This is deliberately the REQUEST seam, so enrollment is conservative: a
  // draw that later fails still leaves the page claimed. Over-stamping makes
  // the stop signal too loud, which is the only safe direction for it to err.
  if (is_zero_copy_ && result) {
    // The provider is read BEFORE the lock: it runs on this same GPU thread and
    // only reads the encoding submission counter, so there is nothing for the
    // lock to order, and holding the callback lock across foreign code would
    // put an unbounded body inside the guest fault path's critical region.
    uint64_t submission =
        overlap_submission_provider_ ? overlap_submission_provider_() : 0;
    // THE SYNCHRONIZATION EDGE. The stamp is published here and consumed on the
    // faulting guest thread inside the physical invalidation callback. Relaxed
    // atomics on the slot alone give no happens-before between the two: a
    // callback that begins after this enrollment could still legally read the
    // old zero stamp and classify a genuinely raced page as idle -- silence in
    // the one direction this pre-gate may not err in.
    //
    // Taking the SAME lock the callback dispatcher already holds makes the
    // order causal. A callback that ran while holding it happened BEFORE this
    // enrollment and is entitled to see the page idle; a callback that acquires
    // it after this scope MUST see the stamp. The faulting thread takes no new
    // lock for this -- it already holds the only one involved -- and still does
    // just one immutable provider load plus bounded atomics.
    //
    // Strictly AFTER RequestRange, which may run a hazard flush (commit + wait)
    // and takes this region itself: holding it across that would invert nothing
    // but would park an unbounded GPU wait inside the guest's critical region.
    // The stamp is a bounded atomic write; the fork took the memory interface's callback lock around it, which this tree's
    // interface does not publish.
    zero_copy_overlap_.StampConsumer(start, length, submission);
  }
  return NoteConsumed(start, length, result);
}

bool MetalSharedMemory::ConsumedPageIndexEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_CONSUMED_PAGE_INDEX");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

void MetalSharedMemory::IndexConsumedRange(uint32_t index) {
  if (!ConsumedPageIndexEnabled()) {
    return;
  }
  const ConsumedRange& r = consumed_ranges_[index];
  if (r.second <= r.first) {
    return;
  }
  if (consumed_pages_.empty()) {
    consumed_pages_.resize(size_t(kBufferSize >> kConsumedPageShift));
  }
  const uint32_t first = r.first >> kConsumedPageShift;
  const uint32_t last = std::min(uint32_t((r.second - 1) >> kConsumedPageShift),
                                 uint32_t(consumed_pages_.size() - 1));
  for (uint32_t page = first; page <= last; ++page) {
    std::vector<uint32_t>& list = consumed_pages_[page];
    if (list.empty()) {
      consumed_pages_touched_.push_back(page);
    }
    list.push_back(index);
  }
}

void MetalSharedMemory::ClearConsumedPages() {
  for (uint32_t page : consumed_pages_touched_) {
    consumed_pages_[page].clear();
  }
  consumed_pages_touched_.clear();
}

void MetalSharedMemory::AppendConsumedRange(uint32_t start, uint32_t end, uint64_t submission) {
  const uint32_t index = uint32_t(consumed_ranges_.size());
  consumed_index_.Insert((uint64_t(start) << 32) | end, index, true);
  consumed_ranges_.push_back(ConsumedRange{start, end, submission});
  IndexConsumedRange(index);
}

void MetalSharedMemory::NoteGpuWriteInFlight(uint32_t start, uint32_t length,
                                             uint64_t submission) {
  if (!length || !submission) {
    return;
  }
  const uint32_t end = start + length;
  if (const uint32_t* it =
          consumed_index_.Find((uint64_t(start) << 32) | end)) {
    ConsumedRange& range = consumed_ranges_[*it];
    range.submission = std::max(range.submission, submission);
    return;
  }
  AppendConsumedRange(start, end, submission);
}

bool MetalSharedMemory::OnDemandReadbackEnabled() {
  // XE_METAL_DISCRETE_READBACK=eager: every GPU-produced range is read back
  // with its frame (the first discrete-mirror form).
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_DISCRETE_READBACK");
    return !(v && std::strcmp(v, "eager") == 0);
  }();
  return s_enabled;
}

void MetalSharedMemory::NoteCpuWrite(uint32_t offset, uint32_t length) {
  if (!gpu_buffer_ || !length) {
    return;
  }
  {
    // The CPU's bytes are newer than VRAM's, except under a producer in the
    // open submission: the upload's side command buffer runs ahead of it, so
    // that producer's output lands over them in VRAM afterwards.
    std::lock_guard<std::mutex> lock(vram_mutex_);
    if (!vram_newer_.empty()) {
      const uint64_t open =
          vram_encoding_submission_ ? vram_encoding_submission_() : UINT64_MAX;
      SubtractVramNewerLocked(offset, uint64_t(offset) + length,
                              [open](const VramNewer& e) {
                                return !e.eager && e.submission < open;
                              });
    }
  }
  if (cpu_write_callback_) {
    cpu_write_callback_(offset, length);
  }
}

template <typename Pred>
void MetalSharedMemory::SubtractVramNewerLocked(uint32_t start, uint64_t end,
                                                Pred pred) {
  std::vector<VramNewer> tails;
  for (size_t i = 0; i < vram_newer_.size();) {
    VramNewer& e = vram_newer_[i];
    if (e.end <= start || e.start >= end || !pred(e)) {
      ++i;
      continue;
    }
    const bool keep_head = e.start < start;
    const bool keep_tail = e.end > end;
    if (keep_head && keep_tail) {
      VramNewer tail = e;
      tail.start = uint32_t(end);
      tails.push_back(tail);
      e.end = start;
    } else if (keep_head) {
      e.end = start;
    } else if (keep_tail) {
      e.start = uint32_t(end);
    } else {
      vram_newer_[i] = vram_newer_.back();
      vram_newer_.pop_back();
      continue;
    }
    ++i;
  }
  vram_newer_.insert(vram_newer_.end(), tails.begin(), tails.end());
}

void MetalSharedMemory::PruneVramNewerLocked() {
  // Completed frame readbacks have landed in the mirror. An entry covered by
  // a newer COMMITTED one is superseded: a fetch of those bytes returns the
  // newer producer's output either way, which is what publishing the owners
  // in order leaves in RAM.
  const uint64_t completed =
      vram_completed_submission_ ? vram_completed_submission_() : 0;
  const uint64_t open =
      vram_encoding_submission_ ? vram_encoding_submission_() : UINT64_MAX;
  std::vector<VramNewer> kept;
  kept.reserve(vram_newer_.size());
  for (const VramNewer& e : vram_newer_) {
    if (e.eager && e.submission <= completed) {
      continue;
    }
    bool superseded = false;
    if (!e.eager) {
      for (const VramNewer& f : vram_newer_) {
        if (!f.eager && f.submission > e.submission && f.submission < open &&
            f.start <= e.start && f.end >= e.end) {
          superseded = true;
          break;
        }
      }
    }
    if (!superseded) {
      kept.push_back(e);
    }
  }
  vram_newer_.swap(kept);
  vram_prune_at_ = std::max<size_t>(64, vram_newer_.size() * 2);
}

void MetalSharedMemory::MarkVramNewer(uint32_t start, uint32_t length,
                                      uint64_t submission, bool eager) {
  if (!gpu_buffer_ || !length) {
    return;
  }
  std::lock_guard<std::mutex> lock(vram_mutex_);
  // Same submission, same bytes: one producer execution supersedes the other.
  const uint64_t end = uint64_t(start) + length;
  SubtractVramNewerLocked(start, end, [&](const VramNewer& e) {
    return e.eager == eager && e.submission == submission;
  });
  vram_newer_.push_back(
      VramNewer{start, uint32_t(end), submission, start, eager});
  if (vram_newer_.size() >= vram_prune_at_) {
    PruneVramNewerLocked();
    if (vram_newer_.size() > 8192 && !vram_overflowed_) {
      // Unbounded distinct producers: the command processor falls back to
      // frame readbacks (IsVramHot reports everything hot).
      vram_overflowed_ = true;
      REXLOG_WARN("MetalSharedMemory: {} VRAM-newer ranges; on-demand readback "
             "falls back to frame readbacks",
             vram_newer_.size());
    }
  }
}

bool MetalSharedMemory::IsVramHot(uint32_t origin) const {
  std::lock_guard<std::mutex> lock(vram_mutex_);
  return vram_overflowed_ || vram_hot_.count(origin) != 0;
}

bool MetalSharedMemory::EnsureCpuCurrent(uint32_t start, uint32_t length) {
  if (!gpu_buffer_ || !fetch_queue_ || !length) {
    return true;
  }
  struct Piece {
    uint32_t start, end;
    uint64_t submission;
    uint32_t origin;
    size_t span;
  };
  std::vector<Piece> pieces;
  uint64_t wait_submission = 0;
  const uint64_t request_end = uint64_t(start) + length;
  {
    std::lock_guard<std::mutex> lock(vram_mutex_);
    if (vram_newer_.empty()) {
      return true;
    }
    const uint64_t open =
        vram_encoding_submission_ ? vram_encoding_submission_() : UINT64_MAX;
    for (const VramNewer& e : vram_newer_) {
      // A producer in the open submission has not run; the mirror already
      // holds what a read before it would see.
      if (e.end <= start || e.start >= request_end || e.submission >= open) {
        continue;
      }
      wait_submission = std::max(wait_submission, e.submission);
      if (!e.eager) {
        pieces.push_back(Piece{std::max(e.start, start),
                               uint32_t(std::min<uint64_t>(e.end, request_end)),
                               e.submission, e.origin, 0});
      }
    }
  }
  if (pieces.empty()) {
    return true;
  }
  const auto t0 = std::chrono::steady_clock::now();
  if (vram_wait_ && !vram_wait_(wait_submission)) {
    return false;
  }
  // Word-aligned spans covering the pieces, packed into one staging buffer.
  std::sort(pieces.begin(), pieces.end(),
            [](const Piece& a, const Piece& b) { return a.start < b.start; });
  struct Span {
    uint32_t start, end, staging;
  };
  std::vector<Span> spans;
  const uint64_t limit = gpu_visible_length_;
  for (Piece& p : pieces) {
    uint32_t s = p.start & ~3u;
    uint32_t e = uint32_t(std::min<uint64_t>((uint64_t(p.end) + 3) & ~3ull,
                                             limit));
    if (!spans.empty() && s <= spans.back().end) {
      spans.back().end = std::max(spans.back().end, e);
    } else {
      spans.push_back(Span{s, e, 0});
    }
    p.span = spans.size() - 1;
  }
  uint32_t total = 0;
  for (Span& s : spans) {
    s.staging = total;
    total += s.end - s.start;
  }
  @autoreleasepool {
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    id<MTLBuffer> staging =
        [device newBufferWithLength:total options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)fetch_queue_;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!staging || !blit) {
      return false;
    }
    blit.label = @"shared memory fetch";
    id<MTLBuffer> gpu = (__bridge id<MTLBuffer>)gpu_buffer_;
    for (const Span& s : spans) {
      [blit copyFromBuffer:gpu
              sourceOffset:s.start
                  toBuffer:staging
         destinationOffset:s.staging
                      size:s.end - s.start];
    }
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
      REXLOG_WARN("MetalSharedMemory: VRAM fetch failed (status {})",
             int(cb.status));
      return false;
    }
    const uint8_t* fetched = static_cast<const uint8_t*>(staging.contents);
    std::lock_guard<std::mutex> lock(vram_mutex_);
    uint64_t copied = 0;
    for (const Piece& p : pieces) {
      const Span& span = spans[p.span];
      // Copy only what is still recorded for this producer: bytes the CPU
      // rewrote meanwhile were subtracted by NoteCpuWrite.
      for (const VramNewer& e : vram_newer_) {
        if (e.eager || e.submission != p.submission || e.origin != p.origin ||
            e.end <= p.start || e.start >= p.end) {
          continue;
        }
        uint32_t c0 = std::max(e.start, p.start);
        uint32_t c1 = std::min(e.end, p.end);
        std::memcpy(buffer_contents_ + c0,
                    fetched + span.staging + (c0 - span.start), c1 - c0);
        copied += c1 - c0;
      }
      SubtractVramNewerLocked(p.start, p.end, [&](const VramNewer& e) {
        return !e.eager && e.submission == p.submission &&
               e.origin == p.origin;
      });
      vram_hot_.insert(p.origin);
    }
    ++vram_stats_.fetches;
    vram_stats_.bytes += copied;
    vram_stats_.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
  }
  return true;
}

MetalSharedMemory::VramFetchStats MetalSharedMemory::ConsumeVramFetchStats() {
  std::lock_guard<std::mutex> lock(vram_mutex_);
  VramFetchStats stats = vram_stats_;
  stats.entries = vram_newer_.size();
  stats.hot = vram_hot_.size();
  vram_stats_ = VramFetchStats{};
  return stats;
}

void MetalSharedMemory::RebuildConsumedIndex() {
  consumed_index_.Clear();
  consumed_index_.Reserve(consumed_ranges_.size());
  ClearConsumedPages();
  for (uint32_t i = 0; i < consumed_ranges_.size(); ++i) {
    const ConsumedRange& r = consumed_ranges_[i];
    consumed_index_.Insert((uint64_t(r.first) << 32) | r.second, i, false);
    IndexConsumedRange(i);
  }
}

bool MetalSharedMemory::NoteConsumed(uint32_t start, uint32_t length,
                                     bool result) {
  uint32_t end = start + length;
  uint64_t submission = submission_provider_ ? submission_provider_() : 0;
  {
    const uint32_t* hit =
        consumed_index_.Find((uint64_t(start) << 32) | end);
    if (hit && *hit < consumed_ranges_.size()) {
      ConsumedRange& range = consumed_ranges_[*hit];
      if (range.first == start && range.second == end) {
        range.submission = std::max(range.submission, submission);
        return result;
      }
    }
  }
  if (ConsumedPageIndexEnabled()) {
    // A containing range covers start's page; the list is ascending, so the
    // first match is the one the scan below would find.
    if (!consumed_pages_.empty() && (start >> kConsumedPageShift) < consumed_pages_.size()) {
      for (uint32_t i : consumed_pages_[start >> kConsumedPageShift]) {
        ConsumedRange& range = consumed_ranges_[i];
        if (start >= range.first && end <= range.second) {
          range.submission = std::max(range.submission, submission);
          return result;
        }
      }
    }
  } else {
    for (auto& range : consumed_ranges_) {
      if (start >= range.first && end <= range.second) {
        // Already covered — but the NEWER submission now also reads it; keep
        // the range protected until the newest reader completes.
        range.submission = std::max(range.submission, submission);
        return result;
      }
    }
  }
  AppendConsumedRange(start, end, submission);
  return result;
}

bool MetalSharedMemory::DiscreteMirrorEnabled() {
  static const bool s_enabled = [] {
    if (const char* v = std::getenv("XE_METAL_DISCRETE_MIRROR")) {
      return v[0] == '1';
    }
    return GetMetalSettings().discrete_vram_mirror.Get();
  }();
  return s_enabled;
}

bool MetalSharedMemory::SharedMirrorCachedEnabled() {
  // Default on (2026-11-02); XE_METAL_SHARED_MIRROR_CACHED=0 is the exact
  // rollback to the write-combined unified mirror.
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_SHARED_MIRROR_CACHED");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

bool MetalSharedMemory::VfetchRenameEnabled() {
  // Default on (2026-10-05 gate); XE_METAL_VFETCH_RENAME=0 is the exact
  // rollback to the staged (pass-ending) hazard upload.
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_VFETCH_RENAME");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

bool MetalSharedMemory::RequestVertexFetchRange(uint32_t start, uint32_t length,
                                                VfetchRename* rename_out) {
  *rename_out = VfetchRename{};
  if (!length) {
    return true;
  }
  rename_context_ = true;
  rename_request_start_ = start;
  rename_request_length_ = length;
  rename_happened_ = false;
  bool result = RequestRange(start, length);
  rename_context_ = false;
  if (result && rename_happened_) {
    *rename_out = rename_result_;
    return true;
  }
  if (is_zero_copy_ && result) {
    uint64_t submission =
        overlap_submission_provider_ ? overlap_submission_provider_() : 0;
    zero_copy_overlap_.StampConsumer(start, length, submission);
  }
  return NoteConsumed(start, length, result);
}

bool MetalSharedMemory::AllocateRename(uint32_t length,
                                       uint32_t* arena_offset_out) {
  const uint64_t completed =
      completed_submission_provider_ ? completed_submission_provider_() : 0;
  while (!rename_fences_.empty() &&
         rename_fences_.front().submission <= completed) {
    rename_fences_.pop_front();
  }
  length = (length + 255u) & ~255u;
  if (!length || length > kRenameArenaBytes) {
    return false;
  }
  uint32_t start = rename_head_;
  if (start + length > kRenameArenaBytes) {
    start = 0;
  }
  const uint32_t end = start + length;
  for (const RenameFence& fence : rename_fences_) {
    if (start < fence.end && end > fence.start) {
      return false;  // still in flight: the caller takes the staged route.
    }
  }
  const uint64_t submission = submission_provider_ ? submission_provider_() : 0;
  if (!rename_fences_.empty() &&
      rename_fences_.back().submission == submission &&
      rename_fences_.back().end == start) {
    rename_fences_.back().end = end;
  } else {
    rename_fences_.push_back({start, end, submission});
  }
  rename_head_ = end;
  *arena_offset_out = start;
  return true;
}

void MetalSharedMemory::ReleaseConsumedRanges(uint64_t completed_submission) {
  const uint32_t count = uint32_t(consumed_ranges_.size());
  uint32_t first_removed = 0;
  while (first_removed < count &&
         consumed_ranges_[first_removed].submission > completed_submission) {
    ++first_removed;
  }
  if (first_removed == count) {
    return;
  }
  // Compact in place and remap both indices: survivors keep their order, so
  // every page list stays ascending. Until 2026-10-19 any release rebuilt
  // both from scratch, freeing and reallocating every hash node, once per
  // completed submission (Burnout: ~11 a frame, 1.5% of the CP thread). The
  // hash is only a shortcut (a miss falls back to the page scan), so a
  // duplicate key losing its entry costs a scan, never a wrong answer.
  // XE_METAL_CONSUMED_REBUILD=1 restores the rebuild.
  static const bool s_rebuild = [] {
    const char* v = std::getenv("XE_METAL_CONSUMED_REBUILD");
    return v && v[0] == '1' && !v[1];
  }();
  if (s_rebuild) {
    consumed_ranges_.erase(
        std::remove_if(consumed_ranges_.begin(), consumed_ranges_.end(),
                       [&](const ConsumedRange& range) {
                         return range.submission <= completed_submission;
                       }),
        consumed_ranges_.end());
    RebuildConsumedIndex();
    return;
  }
  constexpr uint32_t kRemoved = UINT32_MAX;
  consumed_remap_.resize(count);
  for (uint32_t i = 0; i < first_removed; ++i) {
    consumed_remap_[i] = i;
  }
  uint32_t kept = first_removed;
  for (uint32_t i = first_removed; i < count; ++i) {
    const ConsumedRange range = consumed_ranges_[i];
    if (range.submission <= completed_submission) {
      consumed_remap_[i] = kRemoved;
      continue;
    }
    consumed_ranges_[kept] = range;
    consumed_remap_[i] = kept++;
  }
  consumed_ranges_.resize(kept);
  // The flat map has no erase; refilling it keeps its storage (no allocation)
  // and, like AppendConsumedRange, lets the newest range own a duplicate key.
  consumed_index_.Clear();
  for (uint32_t i = 0; i < kept; ++i) {
    const ConsumedRange& range = consumed_ranges_[i];
    consumed_index_.Insert((uint64_t(range.first) << 32) | range.second, i,
                           true);
  }
  if (ConsumedPageIndexEnabled()) {
    size_t pages_kept = 0;
    for (uint32_t page : consumed_pages_touched_) {
      std::vector<uint32_t>& list = consumed_pages_[page];
      size_t out = 0;
      for (uint32_t index : list) {
        const uint32_t mapped = consumed_remap_[index];
        if (mapped != kRemoved) {
          list[out++] = mapped;
        }
      }
      list.resize(out);
      if (out) {
        consumed_pages_touched_[pages_kept++] = page;
      }
    }
    consumed_pages_touched_.resize(pages_kept);
  }
}

ZeroCopyOverlapSnapshot MetalSharedMemory::zero_copy_overlap_snapshot() const {
  // Same lock as the enrollment and the callback, so what the reporter prints
  // is one coherent moment instead of counters sampled at different instants
  // (which could show, say, more classified pages than observed callbacks and
  // read as a defect in the model). Bounded and allocation-free inside: a
  // fixed-size struct copy, no logging -- the caller formats after this
  // returns, never inside the guest fault path's critical region.
  return zero_copy_overlap_.Snapshot();
}

void MetalSharedMemory::ObservePhysicalCpuWrite(
    uint32_t physical_address_start, uint32_t length) {
  // Alias guard (task 5.2). Exact-off and every fallback arm return here
  // BEFORE any provider is invoked, so "the control arm did no zero-copy work"
  // is observable at the provider rather than inferred from an empty counter.
  if (!is_zero_copy_) {
    return;
  }
  // ONE completed high-water mark for the whole callback, loaded once from the
  // independent completion source. Sampling it per page would let a submission
  // complete mid-range and split one physical write across two different
  // verdicts, so a page could be reported finished only because the loop
  // reached it late.
  //
  // Everything below this line runs on the faulting guest thread WITH the
  // global critical region already held (PhysicalHeap::TriggerCallbacks walks
  // the callback list under it), i.e. on the critical path of every guest
  // fault: one acquire load through an immutable provider, then bounded atomic
  // work in the observer. No wait, no log, no allocation, NO NEW LOCK, no
  // command-processor mutation, no other command-processor API.
  //
  // The stamps this reads are published under that SAME already-held lock by
  // RequestConsumedRange, so the ordering is causal rather than hopeful: this
  // callback either ran before an enrollment (and may see the page idle, which
  // was true when it ran) or acquired the lock after it (and must see the
  // stamp). Nothing extra is paid here for that edge -- the lock is already in
  // hand -- which is why the producer, not the consumer, is the side that had
  // to change.
  uint64_t completed_submission = overlap_completed_submission_provider_
                                      ? overlap_completed_submission_provider_()
                                      : 0;
  zero_copy_overlap_.ObserveCpuWrite(physical_address_start, length,
                                     completed_submission);
}

void MetalSharedMemory::ClearCache() {
  // Hazard records name byte ranges consumed by draws in a submission, so
  // their only purpose is protecting encoded-but-unfinished readers. At the
  // clear boundary there are none, and ReleaseConsumedRanges (driven by the
  // pumped completion actions) should already have emptied the set -- which
  // the caller VERIFIES before calling this, so clearing here cannot erase
  // the evidence for that check.
  ResetFrameRanges();
  ::rex::gpu::SharedMemory::ClearCache();
}

SharedBufferConvergenceOutcome MetalSharedMemory::ConvergeRangeToRam(
    uint32_t base, uint32_t length) {
  SharedBufferConvergenceOutcome outcome;
  if (!buffer_contents_) {
    // No backing at all (pre-Initialize / post-Shutdown). Report the factual
    // disagreement so the caller retains its pending record rather than
    // retiring bytes that were never published.
    return outcome;
  }
  // Domain checks BEFORE any pointer arithmetic. `base + length` is formed
  // nowhere below in a way that could wrap: the subtraction is on values
  // already proven to be in range. A zero length is rejected rather than
  // silently "succeeding" -- an empty publication that reported converged would
  // retire a record whose bytes nothing ever moved, and the caller cannot tell
  // that apart from a real alias skip.
  if (!length || base >= kBufferSize || length > kBufferSize - base) {
    return outcome;
  }
  uint8_t* destination = TranslateGuestAddress<uint8_t*>(base);
  if (!destination) {
    return outcome;
  }
  const uint8_t* source = buffer_contents_ + base;
  EnsureCpuCurrent(base, length);
  if (is_zero_copy_) {
    // A claimed alias is not evidence. Skipping the copy is licensed only by
    // the destination BEING the source at this exact range; if the two
    // disagree, the copy would be a real publication and suppressing it would
    // silently lose the export, so this fails and the caller retries.
    if (!ZeroCopySelfCopyIsProven(is_zero_copy_, destination, source)) {
      return outcome;
    }
    outcome.alias_skipped = true;
    outcome.bytes_copied = 0;  // proven self-copy: no byte moved.
    outcome.converged = true;
    return outcome;
  }
  std::memcpy(destination, source, length);
  outcome.bytes_copied = length;  // recorded AT the copy that moved them.
  outcome.converged = true;
  return outcome;
}

SharedBufferConvergenceOutcome MetalSharedMemory::ConvergeChangedWordsToRam(
    uint32_t base, uint32_t length, const uint8_t* pre_image) {
  if (!pre_image || is_zero_copy_) {
    return ConvergeRangeToRam(base, length);
  }
  SharedBufferConvergenceOutcome outcome;
  if (!buffer_contents_ || !length || base >= kBufferSize ||
      length > kBufferSize - base) {
    return outcome;
  }
  uint8_t* destination = TranslateGuestAddress<uint8_t*>(base);
  if (!destination) {
    return outcome;
  }
  EnsureCpuCurrent(base, length);
  outcome.bytes_copied = metal::CopyChangedWords(
      destination, buffer_contents_ + base, pre_image, length);
  outcome.converged = true;
  return outcome;
}

void MetalSharedMemory::WriteTraceInitialState(TraceWriter& trace_writer) {
  // Nothing to write: this tree has no trace writer, and the range list it used was maintained by one.
  (void)trace_writer;
}

bool MetalSharedMemory::UploadRangesImpl(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  if (!num_upload_ranges) {
    return true;
  }
  if (!buffer_contents_) {
    return false;
  }
  // Only invalidated pages reach here, so any overlap with a range a draw in
  // the open frame command buffer consumed is a genuine intra-frame rewrite:
  // flush (commit + wait) BEFORE the new bytes land so those draws read the
  // old data. The callback also resets consumed_ranges_.
  bool hazard = false;
  // True when the overlapping consumer is in the OPEN command buffer: the
  // staged path must commit it first so the blit orders after it. An
  // overlap with an already-committed (in-flight) submission, or with an
  // earlier staged blit, needs no commit: the blit encoded into the open
  // buffer already executes after them.
  bool hazard_in_open_buffer = false;
  uint32_t hazard_upload_start = 0, hazard_upload_end = 0;
  uint32_t hazard_consumed_start = 0, hazard_consumed_end = 0;
  const uint64_t open_submission =
      submission_provider_ ? submission_provider_() : 0;
  // Experiment (XE_METAL_FLUSH_ALL_UPLOADS): flush before ANY mid-frame
  // invalidating upload once a draw has consumed shared memory this frame,
  // ignoring the overlap test. Over-approximates per-draw serialization for the
  // shared-memory path (no arena coupling) — an in-agent A/B for whether the
  // stipple is shared-memory upload staleness at all.
  static const bool s_flush_all =
      std::getenv("XE_METAL_FLUSH_ALL_UPLOADS") != nullptr;
  if (s_flush_all && !consumed_ranges_.empty()) {
    hazard = true;
    hazard_in_open_buffer = true;
  }
  for (uint32_t i = 0; i < num_upload_ranges && !hazard_in_open_buffer; ++i) {
    uint32_t range_start = upload_page_ranges[i].first << GetPageSizeLog2();
    uint32_t range_end = range_start
                         + (upload_page_ranges[i].second << GetPageSizeLog2());
    auto test = [&](const ConsumedRange& consumed) {
      if (range_start < consumed.second && range_end > consumed.first) {
        if (!hazard) {
          hazard = true;
          hazard_upload_start = range_start;
          hazard_upload_end = range_end;
          hazard_consumed_start = consumed.first;
          hazard_consumed_end = consumed.second;
        }
        if (!open_submission || consumed.submission >= open_submission) {
          hazard_in_open_buffer = true;
          return true;
        }
      }
      return false;
    };
    if (ConsumedPageIndexEnabled()) {
      // Candidates from the pages the upload covers, visited in ascending
      // range order like the scan (first-hazard bookkeeping is identical).
      if (!consumed_pages_.empty() && range_end > range_start) {
        upload_candidates_.clear();
        const uint32_t first = range_start >> kConsumedPageShift;
        const uint32_t last = std::min(uint32_t((range_end - 1) >> kConsumedPageShift),
                                       uint32_t(consumed_pages_.size() - 1));
        for (uint32_t page = first; page <= last; ++page) {
          const std::vector<uint32_t>& list = consumed_pages_[page];
          upload_candidates_.insert(upload_candidates_.end(), list.begin(), list.end());
        }
        if (first != last) {
          std::sort(upload_candidates_.begin(), upload_candidates_.end());
          upload_candidates_.erase(
              std::unique(upload_candidates_.begin(), upload_candidates_.end()),
              upload_candidates_.end());
        }
        for (uint32_t c : upload_candidates_) {
          if (test(consumed_ranges_[c])) {
            break;
          }
        }
      }
    } else {
      for (const auto& consumed : consumed_ranges_) {
        if (test(consumed)) {
          break;
        }
      }
    }
  }
  // XE_METAL_HAZARD_STAGED=0 restores the commit + wait for every hazard.
  static const bool s_hazard_staged = [] {
    const char* v = std::getenv("XE_METAL_HAZARD_STAGED");
    return !(v && v[0] == '0' && !v[1]);
  }();
  bool staged = false;
  if (hazard) {
    static uint32_t s_hazard_flushes = 0;
    const bool can_stage = s_hazard_staged && open_submission &&
                           staged_upload_callback_ && hazard_commit_callback_;
    if (s_hazard_flushes++ < 24 || (s_hazard_flushes & 1023) == 0) {
      REXLOG_INFO(
          "MetalSharedMemory: intra-frame rewrite hazard #{} ({}) upload "
          "{:08X}-{:08X} over consumed {:08X}-{:08X} ({} consumed ranges, "
          "open={})",
          s_hazard_flushes,
          can_stage ? (hazard_in_open_buffer ? "commit+stage" : "stage")
                    : "flush+wait",
          hazard_upload_start, hazard_upload_end, hazard_consumed_start,
          hazard_consumed_end, consumed_ranges_.size(),
          hazard_in_open_buffer ? 1 : 0);
    }
    if (rename_context_ && !is_zero_copy_ && gpu_visible_length_ > kBufferSize &&
        rename_request_length_) {
      uint32_t arena_offset = 0;
      const void* source = TranslateGuestAddress<const void*>(rename_request_start_);
      // Guest RAM is the source of the copy, so a range holding GPU output
      // that only the mirror has (a pending resolve writeback or memexport
      // not yet published to RAM: JC2's memexport-fed vertex buffers) must
      // not be renamed; the ordinary path orders the draw after its producer.
      bool gpu_current_overlap = false;
      if (gpu_current_span_provider_) {
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        gpu_current_span_provider_(rename_request_start_,
                                   rename_request_length_, &spans);
        gpu_current_overlap = !spans.empty();
      }
      if (!gpu_current_overlap && source &&
          AllocateRename(rename_request_length_, &arena_offset)) {
        std::memcpy(buffer_contents_ + kBufferSize + arena_offset, source,
                    rename_request_length_);
        NoteCpuWrite(kBufferSize + arena_offset, rename_request_length_);
        rename_result_ = {rename_request_start_, rename_request_length_,
                          kBufferSize + arena_offset};
        rename_happened_ = true;
        ++vfetch_renames_;
        vfetch_rename_bytes_ += rename_request_length_;
        static uint32_t s_rename_logs = 0;
        if (s_rename_logs++ < 8) {
          REXLOG_INFO("MetalSharedMemory: vfetch rename #{} {:08X}+{:X} -> arena+{:X} "
                 "(hazard over {:08X}-{:08X})",
                 vfetch_renames_, rename_request_start_, rename_request_length_,
                 arena_offset, hazard_consumed_start, hazard_consumed_end);
        }
        // The mirror's pages stay invalid: nothing was uploaded for them.
        return true;
      }
      ++vfetch_rename_refusals_;
    }
    if (can_stage) {
      if (hazard_in_open_buffer) {
        hazard_commit_callback_();
      }
      staged = true;
    } else if (hazard_flush_callback_) {
      ++hazard_waits_;
      hazard_flush_callback_();
    }
  }
  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    uint32_t upload_start = upload_page_ranges[i].first << GetPageSizeLog2();
    uint32_t upload_length = upload_page_ranges[i].second << GetPageSizeLog2();
    if (!upload_length) {
      continue;
    }
    // Guest-unallocated pages (2026-09-18): games draw from physical pages
    // the guest heap has released or never allocated — the hardware just
    // reads whatever is there, and xenia's BaseHeap::Release notes the same.
    // The shared refusal (also in Canary's D3D12/Vulkan UploadRanges) exists
    // to keep the host memcpy off pages that were host-protected, which only
    // happens with --protect_on_release. So: refuse only when the host page
    // could actually be unreadable; otherwise upload the bytes as hardware
    // would read them. Dishonored (425307E3) dropped 4-45 draws per frame
    // this way (polygons flashing in and out); Forza Horizon 1/2, Batman AA
    // and Gears 2 dropped a few per frame. XE_METAL_STRICT_UPLOAD_RANGE=1
    // restores the unconditional refusal; --::rex::graphics::metal::GetMetalSettings().gpu_allow_invalid_upload_range.Get()
    // still forces the upload even with protect_on_release.
    static const bool s_strict_upload_range =
        std::getenv("XE_METAL_STRICT_UPLOAD_RANGE") != nullptr;
    if (!GetMetalSettings().allow_invalid_upload_range.Get()) {
      uint32_t last_page_start =
          upload_start + upload_length - (1u << GetPageSizeLog2());
      if (false) {
        if (s_strict_upload_range || GetMetalSettings().protect_on_release.Get()) {
          REXLOG_ERROR("MetalSharedMemory: invalid upload range {:08X}+{:X}",
                 upload_start, upload_length);
          return false;
        }
        static uint32_t s_unallocated_log = 0;
        if (s_unallocated_log < 32) {
          ++s_unallocated_log;
          REXLOG_WARN(
              "MetalSharedMemory: uploading guest-unallocated range "
              "{:08X}+{:X} (host-readable; hardware reads freed memory)",
              upload_start, upload_length);
        }
      }
    }
    // Validate BEFORE the memcpy so a CPU write racing the copy re-invalidates
    // the pages rather than being lost (base-class contract). Alias mode does
    // not change this: the pages are equally "no longer owed an upload", and
    // moving the validation inside the copy branch would leave alias mode
    // re-walking the same pages forever.
    MarkRangeValid(upload_start, upload_length, false);
    // The ONLY alias-mode difference (task 4.1). The CPU already wrote these
    // bytes into the backing the GPU reads, so the copy below would have
    // identical source and destination -- undefined as a memcpy and pointless
    // as work. Validity above and trace recording below are deliberately
    // OUTSIDE this branch: neither is a copy, and both are still owed.
    if (is_zero_copy_) {
      upload_bytes_aliased_ =
          SaturatingAdd(upload_bytes_aliased_, upload_length);
    } else {
      // Clobber hunt (reopen 3.1, XE_METAL_UPLOAD_WATCH=<hex base>): name
      // every upload that copies guest RAM over a watched buffer range —
      // the confirmed mechanism that destroys GPU-current resolve content.
      static const uint64_t s_upload_watch = [] {
        const char* v = std::getenv("XE_METAL_UPLOAD_WATCH");
        return v ? strtoull(v, nullptr, 16) : 0ull;
      }();
      if (s_upload_watch &&
          upload_start < s_upload_watch + ::rex::gpu::SharedMemory::kBufferSize &&
          upload_start + upload_length > s_upload_watch) {
        uint32_t wpage = uint32_t(s_upload_watch) >> GetPageSizeLog2();
        REXLOG_WARN(
            "MetalUploadWatch CLOBBER upload {:08X}+{:X} overlaps watch "
            "{:08X} (watch page {:X}: gpu_written={})",
            upload_start, upload_length, uint64_t(s_upload_watch), wpage,
            IsPageGpuWrittenByBackend(wpage));
      }
      // Pieces of this upload that are NOT pending GPU-current spans: those
      // bytes are the GPU's newest and RAM is stale for them (straddler
      // fix, see SetGpuCurrentSpanProvider).
      std::vector<std::pair<uint32_t, uint32_t>> pieces;
      {
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        if (gpu_current_span_provider_) {
          gpu_current_span_provider_(upload_start, upload_length, &spans);
        }
        std::sort(spans.begin(), spans.end());
        uint32_t cursor = upload_start;
        const uint32_t upload_end = upload_start + upload_length;
        for (const auto& span : spans) {
          uint32_t s0 = std::max(span.first, upload_start);
          uint32_t s1 = std::min(span.first + span.second, upload_end);
          if (s0 >= s1) {
            continue;
          }
          if (s0 > cursor) {
            pieces.emplace_back(cursor, s0 - cursor);
          }
          cursor = std::max(cursor, s1);
        }
        if (cursor < upload_end) {
          pieces.emplace_back(cursor, upload_end - cursor);
        }
        uint64_t skipped = upload_length;
        for (const auto& piece : pieces) {
          skipped -= piece.second;
        }
        upload_bytes_skipped_gpu_current_ =
            SaturatingAdd(upload_bytes_skipped_gpu_current_, skipped);
        static uint32_t s_skip_logs = 0;
        if (skipped && s_skip_logs++ < 16) {
          REXLOG_INFO(
              "MetalSharedMemory: upload {:08X}+{:X} skips {} GPU-current "
              "bytes in {} span(s)",
              upload_start, upload_length, skipped, spans.size());
        }
        // XE_METAL_LINEAGE=<base hex>: every upload covering the watched base
        // reports whether it copies the base's bytes (RAM over the buffer) or
        // skips them as a pending GPU-current span, with both words.
        static const uint32_t s_lineage_base = [] {
          const char* v = std::getenv("XE_METAL_LINEAGE");
          return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
        }();
        if (s_lineage_base && s_lineage_base >= upload_start &&
            s_lineage_base < upload_start + upload_length) {
          bool copies_base = false;
          for (const auto& piece : pieces) {
            if (s_lineage_base >= piece.first &&
                s_lineage_base < piece.first + piece.second) {
              copies_base = true;
            }
          }
          uint32_t ram_word = 0, buf_word = 0;
          std::memcpy(&ram_word, TranslateGuestAddress<const void*>(s_lineage_base),
                      sizeof(ram_word));
          std::memcpy(&buf_word, buffer_contents_ + s_lineage_base,
                      sizeof(buf_word));
          REXLOG_WARN(
              "MetalLineage UPLOAD base={:08X} upload={:08X}+{:X} "
              "copies_base={} skipped={} spans={} staged={} ram={:08X} "
              "buf_before={:08X}",
              s_lineage_base, upload_start, upload_length, copies_base,
              skipped, spans.size(), staged, ram_word, buf_word);
        }
      }
      for (const auto& piece : pieces) {
        const uint32_t piece_start = piece.first;
        const uint32_t piece_length = piece.second;
        bool copied_direct = true;
        if (staged) {
          uint64_t blit_submission =
              staged_upload_callback_(piece_start, piece_length);
          if (blit_submission) {
            // The blit's target is consumed by its submission: a later
            // direct memcpy over it before the blit executes would be undone
            // by the blit, so it must stage too (the overlap test sees it).
            AppendConsumedRange(piece_start, piece_start + piece_length, blit_submission);
            // Side-command-buffer texture loads must not read these pages
            // before the blit runs (AnyStagedUploadPending). Entries of
            // earlier, committed submissions are dropped as we go.
            staged_open_.erase(
                std::remove_if(staged_open_.begin(), staged_open_.end(),
                               [&](const StagedRange& s) {
                                 return s.submission < blit_submission;
                               }),
                staged_open_.end());
            staged_open_.push_back(StagedRange{
                piece_start, piece_start + piece_length, blit_submission});
            ++staged_uploads_;
            staged_upload_bytes_ =
                SaturatingAdd(staged_upload_bytes_, piece_length);
            copied_direct = false;
          } else if (hazard_flush_callback_) {
            // Could not stage: the historical commit + wait, then the memcpy.
            ++hazard_waits_;
            hazard_flush_callback_();
            staged = false;
          }
        }
        if (copied_direct) {
          NoteCpuWrite(piece_start, piece_length);
          std::memcpy(buffer_contents_ + piece_start,
                      TranslateGuestAddress<const void*>(piece_start), piece_length);
        }
      }
      // Counted AT the copy, so the number is what moved, not what the mode
      // predicts would move.
      upload_bytes_copied_ =
          SaturatingAdd(upload_bytes_copied_, upload_length);
    }
    // Frame-trace recording: the uploaded source bytes are exactly what a
    // replay must restore before this range's consumers run. No-op unless a
    // trace file is open.
    if (trace_writer_) {
      // Publish pending GPU-current bytes in this range to RAM first: the
      // record must hold what the GPU consumes, not the stale RAM behind an
      // unpublished resolve (see SetTracePublishCallback).
      if (trace_publish_callback_) {
        trace_publish_callback_(upload_start, upload_length);
      }
      // No trace writer in this tree; the publish callback above is what records the range.
    }
  }
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
