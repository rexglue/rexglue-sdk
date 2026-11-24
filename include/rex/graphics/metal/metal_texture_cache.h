/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/xenos/shared_memory.h"  // ::rex::gpu::SharedMemory base: it owns guest memory now
#include "rex/graphics/metal/metal_guest_range.h"
#include "rex/graphics/metal/metal_resolved_fetch_service.h"
#include <rex/graphics/metal/metal_shared_memory.h>
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
class TraceWriter;
namespace metal {

// Bring-up minimal texture cache. Given a guest texture fetch constant, produces
// a host MTLTexture (CPU-untiled) and assigns it a slot in the bindless view
// heap, returning that slot so the draw path can write it into the b4
// descriptor-indices cbuffer. Supports only what's needed to get the first real
// textures on screen: 2D k_8_8_8_8 and DXT1/DXT2_3/DXT4_5 (hardware BC on
// Apple Silicon), linear or tiled. Everything else returns slot 0 (the
// null/gray texture).
//
// Deliberately NOT the proper rex::graphics::TextureCache subclass (no shared-memory
// watches, no LRU, no mips, no format table, re-parses each draw but
// caches the host texture by guest key). It exists to prove the texture path;
// the real TextureCache subclass (needs a ::rex::gpu::SharedMemory subclass first) is the
// productionization step.
class MetalTextureCache {
 public:
  // device: id<MTLDevice>. view_heap: id<MTLBuffer> of IRDescriptorTableEntry
  // (the bindless view heap owned by the command processor). Slot 0 is reserved
  // for the null texture.
  // The new interface gives the cache shared memory rather than a Memory pointer: guest bytes are read through it.
  MetalTextureCache(void* device, ::rex::gpu::SharedMemory* shared_memory, void* view_heap,
                    uint32_t view_heap_slots);

  // Guest bytes at a physical address, or nullptr when unreadable. IPhysicalMemory returns a Result of a span.
  const uint8_t* GuestBytes(uint32_t base);
  ~MetalTextureCache();

  // Returns the bindless view-heap slot for the texture described by `fetch`,
  // uploading + caching the host texture on first use. Returns 0 (null) if the
  // format/dimension isn't supported yet or the fetch is invalid. Records the
  // resolved host texture in the active list for residency. `is_signed`
  // requests the SNORM view variant (signed fetches are separate translated
  // bindings expecting -1..1 texels).
  uint32_t GetTextureSlot(const xenos::TextureFetch& fetch,
                          bool is_signed = false);

  // The ONE resolve-backed service classification for a fetch whose base may
  // be a registered resolve target. The serve chain in GetTextureSlot (and
  // any future consumer deciding how a resolve-backed fetch is served)
  // classifies through this — one decision, no duplicated exact-cover/bridge
  // guards. Stable within a draw (resolved_targets_ mutates only at
  // IssueCopy).
  ResolvedFetchService ClassifyFetchService(
      const xenos::TextureFetch& fetch, bool is_signed) const;

  // Lineage probe (XE_METAL_LINEAGE): the command processor stamps the current
  // draw index here each draw so the reader's fetch events share the writer's
  // clock; only read when the probe is active.
  uint32_t lineage_draw_ = 0;

  // RT-as-texture bridge: register a resolved render-target snapshot (owned by
  // the render-target cache) at its guest resolve destination base. A later
  // texture fetch whose base matches samples that snapshot instead of the
  // (never-populated) guest memory, feeding deferred / composite passes. The
  // slot is allocated once per base and reused; the snapshot's contents update
  // in place, so re-registering the same base/texture is a cheap no-op.
  void RegisterResolvedTarget(uint32_t base, void* texture, uint32_t width,
                              uint32_t height, bool is_depth,
                              bool requires_fetch_swizzle,
                              uint32_t guest_extent_start,
                              uint32_t guest_extent_length);

  // Stage 3 hybrid: a resolve at `base` enqueued a guest-RAM writeback, so
  // RAM is authoritative for fetches of this base (the bridge snapshot is a
  // fallback for dests the writer can't encode — e.g. 64bpp HDR).
  void MarkResolvedTargetRamBacked(uint32_t base, uint32_t wb_width,
                                   uint32_t wb_height, bool full_rect_geometry,
                                   uint32_t wb_endian);
  // Draw resolution scaling: the session scale (snapshot textures are the
  // guest size times it), enabling scaled-cover serving above 1x.
  void SetDrawResolutionScale(uint32_t scale_x, uint32_t scale_y) {
    draw_resolution_scale_x_ = scale_x;
    draw_resolution_scale_y_ = scale_y;
  }
  // Draw resolution scaling: a resolve strip that rendered into the snapshot
  // `texture` at guest rows [strip_y, strip_y + rows) and wrote guest bytes
  // [extent_start, + extent_length) with writeback endian wb_endian. Extends
  // the registration's scaled-cover rows when the strip is contiguous with
  // them and was written like the base resolve. No-op at 1x.
  void ExtendResolvedTargetCoverage(void* texture, uint32_t strip_y,
                                    uint32_t rows, uint32_t extent_start,
                                    uint32_t extent_length,
                                    uint32_t wb_endian);
  // Draw resolution scaling: a column resolve rendered into the assembly
  // snapshot `texture` (the destination's full pitch x height, at guest x
  // offset x, width w, full height) and wrote guest bytes [extent_start,
  // + extent_length) with endian wb_endian, after its writeback was encoded.
  // The assembly at base0 serves scaled cover once its columns contiguously
  // cover its width from x = 0. No-op at 1x.
  void RegisterScaledAssembly(uint32_t base0, void* texture, uint32_t width,
                              uint32_t height, uint32_t x, uint32_t w,
                              uint32_t extent_start, uint32_t extent_length,
                              uint32_t wb_endian);
  // XE_METAL_TIP_SNAP_VIEW: attach the derived 1x1 texel-(0,0) copy to the
  // registered resolve target at base (allocates/repoints its bindless slot).
  void RegisterResolvedTipView(uint32_t base, void* texture);
  // Snapshot fit (2026-10-03, Batman AA guard shadows): a bridge-served fetch
  // whose dimensions differ from the snapshot's samples a fetch-sized derived
  // texture holding the snapshot's overlapping texels at (0,0) (zero
  // elsewhere), so normalized coordinates address the same guest texels the
  // RAM path would. UE3 resolves 16 per-caster shadow depth maps of different
  // rect sizes into one 512x512 texture and fetches it as 512x512; serving a
  // 512x320 snapshot to that fetch stretched the map and failed every depth
  // comparison (whole-frustum dark shadows). DOA5 needs the opposite (a
  // 1024x608 fetch must not see a 1280x720 snapshot), which exact-size
  // snapshots already give; the fit covers both. The blit is a transfer
  // (pass boundary), so it is prepared BEFORE a draw's encoder opens
  // (PrepareSnapshotFit) and only consumed at bind time.
  // XE_METAL_SNAPSHOT_FIT=0 serves the raw snapshot as before.
  void SetSnapshotFitBlitter(
      std::function<bool(void* src, void* dst, uint32_t width,
                         uint32_t height)>
          blitter) {
    snapshot_fit_blit_ = std::move(blitter);
  }
  // Zeroes a whole Private texture on the GPU (ordered before the fit blit
  // that follows it); used for heap-allocated fitted views.
  void SetSnapshotZeroer(
      std::function<bool(void* texture, uint32_t width, uint32_t height)>
          zeroer) {
    snapshot_zero_ = std::move(zeroer);
  }
  void PrepareSnapshotFit(const xenos::TextureFetch& fetch,
                          bool is_signed);

  // True if a fetch of (base, width, height) will be served by the bridge
  // snapshot rather than guest RAM — the command processor skips the
  // pending-writeback flush (a full mid-frame commit+wait) for such fetches.
  // Must mirror the serving rule in the fetch path's bridge block (covered
  // fast path is R32Float only — see the comment there). Defined in the .mm
  // (needs the MTLTexture bridge cast).
  // `pitch` is the fetch pitch in texels (0: the width, 32-aligned).
  bool BridgeServes(uint32_t base, uint32_t width, uint32_t height,
                    uint32_t pitch = 0) const;

  // Host textures (id<MTLTexture>) referenced since the last ResetActive, to be
  // made resident on the encoder.
  const std::vector<void*>& active_textures() const { return active_textures_; }
  // Bumped by every change to the resolved-target registry (registration,
  // tip views, coverage, RAM-backed marking, retirement, invalidation). The
  // per-draw texture-dependency memo keys on it.
  uint64_t registry_epoch() const { return registry_epoch_; }
  // Bumped whenever a fetch could resolve to a different slot or texture:
  // a descriptor written, a slot retired, a cache entry erased. With
  // registry_epoch() and no pending invalidations, the same fetch constant
  // resolves to the same slot (the command processor's binding memo).
  uint64_t binding_generation() const { return binding_generation_; }
  bool HasPendingInvalidations() const {
    return pending_invalidations_nonempty_.load(std::memory_order_acquire);
  }
  void ResetActive() {
    active_textures_.clear();
    binding_allocation_failed_ = false;
  }
  bool binding_allocation_failed() const { return binding_allocation_failed_; }
  void ClearBindingAllocationFailure() { binding_allocation_failed_ = false; }

  // Appends EVERY live texture a bindless slot can reach (guest cache entries
  // + resolved targets) for the per-encoder residency sweep: a shader indexing
  // a slot whose texture was only made resident on an EARLIER encoder (stale
  // descriptor index, prior-generation slot) page-faults otherwise — the
  // attributed live faults ('draws@N' encoders) are exactly this class.
  void AppendAllTextures(std::vector<void*>& out) const;
  // Census: bytes (allocatedSize) and counts of the owned textures (views
  // excluded): cache entries, parked entries, resolved-target textures.
  struct ByteCensus {
    uint64_t cache_bytes = 0, cache_count = 0, parked_bytes = 0,
             parked_count = 0, resolved_bytes = 0, resolved_count = 0,
             retired_bytes = 0, retired_count = 0;
  };
  ByteCensus CensusBytes() const;
  // Retired-but-not-yet-released textures (in-flight submissions may still
  // reference them through stale slots): residency-set reconciliation keeps
  // them resident until the drain releases them.
  void AppendRetiredTextures(std::vector<void*>& out) const {
    for (const auto& entry : retired_textures_) {
      if (entry.second) {
        out.push_back(entry.second);
      }
    }
  }

  // Returns bindless slots retired by submissions <= `completed_submission`
  // (a resolved target got a new texture generation -> new slot) to the free
  // list, and releases textures retired by InvalidateRange in that window.
  // Entries retired while ENCODING a later, still-in-flight submission stay
  // retired — freeing them page-faults the in-flight draws (the async-S8
  // black flicker). Pass UINT64_MAX at shutdown.
  void ReclaimStaleSlots(uint64_t completed_submission);

  // Tags retires with the submission that may last reference them.
  void SetSubmissionProvider(std::function<uint64_t()> provider) {
    submission_provider_ = std::move(provider);
  }

  // Stage 3: a resolve wrote [base, base+length) of guest RAM — evict cached
  // textures overlapping it so the next fetch re-uploads fresh content.
  // Evicted textures/slots are retired (freed via ReclaimStaleSlots after the
  // frame's GPU work completes). Returns entries evicted.
  enum class InvalidationScope {
    // An independent RAM writer supersedes every cached representation of the
    // bytes, including an older resolve-backed snapshot bridge.
    kAllRepresentations,
    // The resolve publishing these bytes owns the matching snapshot, so only
    // RAM-derived uploads are stale; preserve the GPU-ordered bridge.
    kRamUploadsOnly,
  };
  // `site` names the caller for the XE_METAL_UNTILE_TIMING eviction census
  // (per-frame calls / evicted entries / bytes per site).
  uint32_t InvalidateRange(
      uint32_t base, uint32_t length,
      InvalidationScope scope = InvalidationScope::kAllRepresentations,
      const char* site = "other");

  // Trace playback restores guest ranges explicitly and calls InvalidateRange
  // for each restore. It doesn't need live CPU page watching, which would add
  // a redundant protected-page copy to every replayed texture upload.
  void SetTracePlayback();

  // Frame-trace recording: uploads record their guest source ranges into an
  // open trace; WriteAllCachedTexturesToTrace records the ranges of WARM
  // entries (they won't re-upload during the traced frame, but the replay
  // must still restore their data). Guest RAM holds their current content.
  void SetTraceWriter(TraceWriter* trace_writer) {
    trace_writer_ = trace_writer;
  }
  void WriteAllCachedTexturesToTrace(TraceWriter& trace_writer) const;

  // Telemetry: bindless-slot pressure (frame stats). Exhaustion is silent
  // at the allocation sites — surfacing it here caught the slot leak that
  // zeroed the exposure pyramid ~1 minute into every session.
  // The view-heap null texture (id<MTLTexture>, not owned). Initialization
  // binds EVERY slot to it so any index the shader can form is a valid
  // resource; a freed slot must be returned to that state rather than left
  // describing a released texture.
  void SetNullTexture(void* null_texture) { null_texture_ = null_texture; }

  // Cache-clear release (implement-metal-cache-clearing step 5). Frees every
  // cached texture, returns its bindless slot to the free list, rebinds the
  // freed view-heap descriptors to the null texture, and drains the
  // submission-gated retire lists outright.
  //
  // Direct release rather than retirement is correct ONLY because the deferred
  // clear runs at a fully quiesced boundary -- encoder closed, every
  // submission complete, completion actions pumped -- which the admission
  // predicate is what guarantees. Never call this from anywhere else.
  //
  // Call ReleaseDerivedResolveBridges BEFORE this: it retires bridge slots and
  // views into the same pools this drains.
  void ClearCache();

  // Diagnostic (XE_METAL_TEX_STALE_VERIFY=<swaps>): hashes each cached
  // entry's guest pages and reports any page that changed since the previous
  // scan while the same texture stayed cached -- a CPU write the watch never
  // reported. Called at a swap on the GPU thread.
  void VerifyCachedAgainstGuest(uint64_t swap);

  // Cache-clear step: release the resolve-snapshot bridges that are DERIVED,
  // and report (destination base, snapshot texture) for each so the
  // render-target cache can release exactly the texture THIS bridge named.
  //
  // The texture identity is reported, not just the base, because
  // EnsureResolveSnapshot replaces the texture at a key when a later resolve
  // needs a larger one (retiring the old to the deferred list). A bridge can
  // therefore hold an older pointer than the snapshot currently at its base,
  // and releasing by base alone would destroy the NEWER snapshot that the
  // present path or another consumer may still be using.
  //
  // Classification:
  //   RELEASED  generation_ram_backed && !is_depth -- a guest-RAM writeback
  //             for THIS snapshot generation has been enqueued, and the clear
  //             publishes every pending writeback before this runs, so guest
  //             RAM holds the content. These are exactly the bridges whose
  //             serve path already reads RAM rather than the snapshot.
  //   RETAINED  !generation_ram_backed -- this generation's content was never
  //             written to guest RAM (either no writeback was ever enqueued
  //             for the destination -- the 64bpp/HDR dests the writer skips --
  //             or a newer snapshot replaced the one that was written back).
  //             The snapshot is then the ONLY copy of that content, and
  //             releasing it would destroy guest-visible data.
  //   RETAINED  is_depth -- depth bridges are served to fetches from the
  //             snapshot even when a writeback exists, so releasing one would
  //             change which content a fetch observes.
  // After a clear, every remaining bridge satisfies
  // (!generation_ram_backed || is_depth).
  void ReleaseDerivedResolveBridges(
      std::vector<std::pair<uint32_t, void*>>* released_out);

  // Replay-boundary retirement (fix-metal-trace-replay-state-reset, carrier
  // C2): retires EVERY resolved-target bridge -- slots, tip slot, and
  // cache-owned swizzle views into the retire pools ClearCache drains -- and
  // erases the map, with NO live retention rules. The clear path's retention
  // above ("the snapshot is the only copy of guest-visible content") is
  // exactly inverted under replay restoration: authoritative RAM + EDRAM are
  // about to be re-delivered by the trace, so a snapshot that is the only
  // copy of PRE-RESTORE content is precisely the state that must die.
  // Snapshot/tip textures are owned elsewhere and are released by their
  // owners at the same seam. Call ONLY from the quiesced replay-reset
  // boundary (MetalCommandProcessor::ResetDerivedReplayState), followed by
  // ClearCache() to drain what this retires.
  void RetireAllResolvedTargets();
  // Retires every bridge (resolved target or scaled assembly) still naming
  // `texture`, a snapshot its owner is about to free. A replaced snapshot
  // whose replacing resolve never re-registered the base would otherwise
  // leave the bridge (and its bindless slot) naming freed memory. Returns
  // whether any bridge named it.
  bool RetireResolvedTargetsNaming(void* texture);
  // Post-clear consistency evidence (a clear must return these toward their
  // post-initialization baseline, and leave nothing queued).
  uint32_t cached_texture_count() const { return uint32_t(cache_.size()); }
  uint32_t retired_texture_count() const {
    return uint32_t(retired_textures_.size());
  }
  uint32_t parked_texture_count() const { return uint32_t(parked_.size()); }
  uint32_t resolved_bridge_count() const {
    return uint32_t(resolved_targets_.size());
  }
  // True when every retained bridge satisfies the retention invariant above.
  bool retained_bridges_are_authoritative() const {
    for (const auto& pair : resolved_targets_) {
      if (pair.second.generation_ram_backed && !pair.second.is_depth) {
        return false;
      }
    }
    return true;
  }

  uint32_t slots_used() const { return next_slot_; }
  // Dimension census (always on, lifetime): unique texture uploads by guest
  // dimension (1D/2D/3D/cube), stacking and mip presence, plus null serves by
  // reason. One line for the frame-stats tick; the boot census greps it.
  std::string FormatDimensionCensus() const;
  // index = dimension (0..3) << 2 | stacked | (mips ? 2 : 0)
  uint32_t dim_census_[16] = {};
  uint32_t dim_census_signed_[16] = {};
  uint32_t null_census_dim_ = 0, null_census_fmt_ = 0,
           null_census_volume_decoded_ = 0, null_census_other_ = 0;
  // CPU-decoded uploads whose guest fetch has a mip chain the legacy path
  // drops (base level only), indexed by guest format (6 bits).
  uint32_t mips_dropped_census_[64] = {};
  // Real 1D textures (texture-typed constant with data) served null, and
  // empty constants that only read as 1D; 3D / stacked uploads whose mip
  // chain is dropped (base level only).
  uint32_t census_1d_real_ = 0, census_1d_empty_ = 0;
  uint32_t census_3d_mips_dropped_ = 0, census_stacked_mips_dropped_ = 0;
  uint32_t slots_free() const { return uint32_t(free_slots_.size()); }
  uint32_t slots_stale() const { return uint32_t(stale_slots_.size()); }
  uint32_t slots_capacity() const { return view_heap_slots_; }


  // Telemetry: uploads performed since the last query (frame stats).
  uint32_t upload_count() const { return upload_count_; }
  uint32_t ConsumeUploadCount() {
    uint32_t n = upload_count_;
    upload_count_ = 0;
    return n;
  }

  // Diagnostic (XE_METAL_UNTILE_TIMING=1, task 0.2 of
  // optimize-metal-gpu-resident-resolve-coherence): wall time spent in the
  // legacy CPU untile/upload miss path since the last query, bucketed by
  // format / signedness / resolve-backed base. Returns a one-line report
  // normalized per frame and resets the accumulators.
  std::string ConsumeUntileTimingReport(uint32_t frames);
  struct UntileTimingBucket {
    uint64_t ns = 0;
    uint32_t count = 0;
    uint64_t bytes = 0;
  };

  // --- C1T-R shared-subclass adapter (single slot/residency authority) ---
  // MetalSharedTextureCache allocates, binds, and retires bindless view-heap
  // slots and registers per-draw residency exclusively through these; a
  // second allocator over the same heap is forbidden (seam audit, proof 1).
  // Residency-set hooks (MetalCommandProcessor): `on_add` fires when a
  // texture becomes reachable through a bindless slot descriptor (the single
  // funnel every sampled texture and view passes), `on_release` when a
  // retired texture is finally released (its last referencing submission
  // completed). Null hooks = the per-encoder residency sweep.
  using ResidencyHook = std::function<void(void*)>;
  void SetResidencyHooks(ResidencyHook on_add, ResidencyHook on_release) {
    residency_add_ = std::move(on_add);
    residency_release_ = std::move(on_release);
  }
  uint32_t AllocateExternalSlot() { return AllocateSlot(); }
  // Points `slot` at a retained id<MTLTexture> the caller keeps alive.
  void BindExternalSlot(uint32_t slot, void* texture_bridge);
  static bool SlotAuditEnabled();
  // XE_METAL_SLOT_AUDIT: checks one draw's texture slots, referenced by
  // `submission`: each must hold a texture registered active for the draw or
  // one in a texture heap (heaps are resident on every encoder).
  void AuditDrawSlots(const uint32_t* slots, size_t count,
                      uint64_t submission);
  void RetireExternalSlot(uint32_t slot) {
    RetireSlot(RetireSubmission(), slot);
  }
  // Takes ownership of a retained id<MTLTexture> (+1); released once the
  // submissions that may reference it complete (same pool as evictions).
  void RetireExternalTexture(void* texture_bridge) {
    retired_textures_.emplace_back(RetireSubmission(), texture_bridge);
  }
  // Per-draw residency registration for a texture served by the subclass.
  void AddActiveTexture(void* texture_bridge) {
    active_textures_.push_back(texture_bridge);
  }

  // Texture heaps (discrete AMD GPUs, which have no MTLResidencySet): Private
  // textures are placed in a few large tracked MTLHeaps so a render encoder
  // makes them all resident with one useHeaps call instead of one
  // useResource per texture per pass (amdMtl_HwCmdBufAddIndirectRsrc was
  // 2.6-4.7% of the command-processor thread). Every release of a heap
  // texture must wait for the submissions that reference it (the retire pool
  // and the snapshot deferred list both do), because useHeaps keeps the heap
  // resident, not the texture alive. XE_METAL_TEXTURE_HEAPS=0/1 overrides.
  void EnableTextureHeaps();
  bool texture_heaps_enabled() const { return heaps_enabled_; }
  // A retained (+1) id<MTLTexture> from a heap, or nullptr when the
  // descriptor is not Private or the heap budget is spent (the caller then
  // allocates from the device as before).
  void* NewHeapTexture(void* descriptor);
  uint32_t texture_heap_count() const {
    return heap_count_.load(std::memory_order_acquire);
  }
  void* texture_heap(uint32_t i) const { return heaps_[i]; }
  uint64_t heap_texture_count() const { return heap_textures_; }
  uint64_t heap_bytes() const { return heap_bytes_; }

 private:
  static constexpr uint32_t kMaxTextureHeaps = 64;
  bool heaps_enabled_ = false;
  void* heaps_[kMaxTextureHeaps] = {};
  std::atomic<uint32_t> heap_count_{0};
  std::mutex heap_mutex_;
  uint64_t heap_budget_ = 0;
  uint64_t heap_bytes_ = 0;
  uint64_t heap_textures_ = 0;
  static void SharedMemoryGlobalWatchCallback(void* context, uint32_t address_first,
                                              uint32_t address_last, bool invalidated_by_gpu);
  void QueueInvalidation(uint32_t address_first, uint32_t address_last);
  void DrainPendingInvalidations();

  // Allocates a bindless view-heap slot, reusing a reclaimed one if available.
  // Returns 0 (the null slot) if the heap is exhausted.
  uint32_t AllocateSlot();

  void* device_;   // id<MTLDevice>, not owned.
  ::rex::gpu::SharedMemory* shared_memory_;  // not owned; owns guest memory
  ::rex::gpu::SharedMemory::GlobalWatchHandle shared_memory_watch_ = nullptr;
  std::mutex pending_invalidations_mutex_;
  // Guest CPU writes may arrive from any emulated thread. The shared-memory
  // callback only queues their ranges; cache mutation and Metal object
  // retirement stay on the GPU thread in DrainPendingInvalidations.
  std::vector<std::pair<uint32_t, uint32_t>> pending_invalidations_;
  // Set under the mutex whenever the queue gains a range and cleared with
  // it: the GPU thread drains on every texture binding, and almost every
  // drain finds the queue empty, so it checks this before taking the lock.
  std::atomic<bool> pending_invalidations_nonempty_{false};
  // The drain swaps the queue with this (capacity kept on both sides).
  std::vector<std::pair<uint32_t, uint32_t>> draining_invalidations_;
  bool trace_playback_ = false;
  void* view_heap_;  // id<MTLBuffer>, not owned.
  void* view_heap_contents_;  // IRDescriptorTableEntry* (CPU pointer).
  void* null_texture_ = nullptr;  // id<MTLTexture>, not owned; may be null.
  uint32_t view_heap_slots_;
  uint32_t next_slot_ = 1;  // 0 == null texture.
  // Distinguishes an intentional null descriptor (unsupported/invalid guest
  // fetch) from failure to create a required Metal object or bindless slot.
  // IssueDraw skips only the latter before emission.
  bool binding_allocation_failed_ = false;
  // Slots freed when a resolved target was repointed to a new texture; the old
  // slot keeps pointing at the old texture until the submissions encoded
  // against it complete, then moves to free_slots_ for reuse. Tagged with the
  // last submission that may reference them.
  // Single writer for every view-heap descriptor. All nine former direct
  // IRDescriptorTableSetTexture sites route through this, so the descriptor
  // write has exactly one implementation.
  void WriteSlotDescriptor(uint32_t slot, void* texture_bridge);
  // XE_METAL_SLOT_AUDIT=1 (diagnostic): what each slot holds and the last
  // submission whose draws index it.
  static std::string DescribeAuditTexture(void* texture_bridge);
  static std::string DescribeAuditCaller(void* address);
  std::vector<void*> audit_slot_texture_;
  std::vector<uint64_t> audit_slot_last_ref_;
  uint64_t audit_completed_ = 0;
  uint32_t audit_live_rewrites_ = 0;
  uint32_t audit_unresident_ = 0;
  std::vector<std::pair<uint64_t, uint32_t>> stale_slots_;
  std::vector<uint32_t> free_slots_;
  // Textures evicted by InvalidateRange, released once their submissions
  // complete.
  std::vector<std::pair<uint64_t, void*>> retired_textures_;
  ResidencyHook residency_add_;
  ResidencyHook residency_release_;
  std::function<uint64_t()> submission_provider_;
  bool BuildResolvedServiceInputs(const xenos::TextureFetch& fetch,
                                  bool is_signed,
                                  ResolvedFetchServiceInputs* out) const;
  // Shared finalize tail (swizzle view, slot, trace record, cache entry) —
  // the ONE place upload finalization semantics live. texture_bridge:
  // id<MTLTexture> at +0. host_format: MTLPixelFormat.
  uint32_t FinalizeTexture(uint64_t key,
                           const xenos::TextureFetch& fetch,
                           void* texture_bridge, uint64_t host_format,
                           uint32_t host_swizzle, bool is_cube,
                           uint32_t host_levels, uint32_t base,
                           uint32_t guest_bytes, uint32_t mip_base,
                           uint32_t mip_bytes);
  uint64_t RetireSubmission() const {
    return submission_provider_ ? submission_provider_() : UINT64_MAX;
  }
  uint32_t upload_count_ = 0;
  // Eviction census (XE_METAL_UNTILE_TIMING=1): per InvalidateRange site.
  struct InvalidationSiteBucket {
    uint32_t calls = 0;
    uint32_t evicted = 0;
    uint64_t bytes = 0;
  };
  std::unordered_map<std::string, InvalidationSiteBucket> inval_census_;
  std::unordered_map<uint32_t, UntileTimingBucket> untile_timing_;
  uint64_t untile_total_ns_ = 0;
  uint32_t untile_total_count_ = 0;
  TraceWriter* trace_writer_ = nullptr;  // not owned; may be null.
  bool bc_supported_ = false;  // Apple Silicon Metal decodes BC1/2/3 in HW.
  // B5G6R5Unorm and the other packed 16-bit formats (Apple GPU families).
  bool packed16_supported_ = true;

  struct Entry {
    void* texture;  // id<MTLTexture>, retained.
    uint32_t slot;
    // Guest byte range the texture was uploaded from — resolve writebacks
    // into this range invalidate the entry (stage 3: fetches read guest RAM).
    uint32_t guest_base;
    uint32_t guest_bytes;
    // Mip range (0/0 when single-level) for invalidation + trace recording.
    uint32_t mip_base = 0;
    uint32_t mip_bytes = 0;
    // XXH3 of the guest bytes (base extent, then mips) at upload time; 0 when
    // revalidation is disabled.
    uint64_t guest_hash = 0;
    // use_clock_ value of the latest insert or hit (capacity eviction).
    uint64_t last_use = 0;
  };
  std::unordered_map<uint64_t, Entry> cache_;
  // Capacity eviction (XE_METAL_TEX_SLOT_LRU=0 disables): when fewer than
  // 1/16 of the bindless slots remain (never-allocated + free; 256 of 4,096),
  // the least-recently-used entries are evicted -- parked for hash revival
  // like an invalidation -- until 3/16 (768) are available or pending
  // reclaim. Without it the cache had no capacity bound: exhaustion handed
  // out slot 0 (the null texture). Red Dead gameplay runs at ~3,980 of 4,096
  // slots and stayed under only because CPU-watch invalidations evicted
  // ~440 textures every frame.
  uint64_t use_clock_ = 0;
  uint64_t lru_evictions_ = 0;
  static bool SlotLruEnabled();
  void EnforceSlotHeadroom();
  // Entry index by 1 MiB guest bucket (XE_METAL_TEX_INVAL_INDEX=0 restores
  // the full scan). A CPU write watch widens to a 64-page block -- 1 MiB with
  // 16 KB host pages -- and InvalidateRange scanned every cache entry for
  // each one: Red Dead gameplay, ~75 invalidations x ~2,800 entries per
  // frame. Every entry whose base or mip range overlaps an invalidation
  // shares a bucket with it, so the candidate set is exact.
  // XE_METAL_TEX_INVAL_INDEX_VERIFY=1 also runs the scan and compares.
  static constexpr uint32_t kEntryBucketShift = 20;
  static constexpr uint32_t kEntryBucketCount =
      uint32_t(metal::GuestRange::kGuestPhysicalSize >> kEntryBucketShift);
  std::vector<std::vector<uint64_t>> entry_buckets_;
  static bool InvalidationIndexEnabled();
  void IndexEntry(uint64_t key, const Entry& entry);
  void UnindexEntry(uint64_t key, const Entry& entry);
  void ClearEntryIndex();
  void EmplaceEntry(uint64_t key, const Entry& entry);
  uint64_t inval_index_verify_calls_ = 0;
  uint64_t inval_index_verify_mismatches_ = 0;

  // Content revalidation (XE_METAL_TEX_REVALIDATE=0 disables): a CPU write
  // watch fires for the whole unwatched region (1 MB in Dead to Rights'
  // texture pool, 16 KB host pages elsewhere), not for the bytes that
  // changed, so an invalidation evicted every texture in the region whether
  // or not its bytes moved. Measured: 366 re-uploads per frame, every one
  // byte-identical, 34 ms of CPU untile per frame at 20 fps. An invalidated
  // entry's texture is PARKED here with the guest hash it was uploaded from
  // (its slot is retired as before); the next fetch of the key re-hashes
  // the guest bytes and re-adopts the texture into a fresh slot when they
  // match, re-arming the write watch. Parked textures are released through
  // the retire list (in-flight draws may still read them).
  struct ParkedTexture {
    void* texture;  // retained +1
    uint64_t guest_hash;
    uint32_t guest_base;
    uint32_t guest_bytes;
    uint32_t mip_base;
    uint32_t mip_bytes;
    uint64_t stamp;
  };
  static constexpr size_t kMaxParkedTextures = 4096;
  std::unordered_map<uint64_t, ParkedTexture> parked_;
  uint64_t parked_stamp_ = 0;
  uint64_t pending_guest_hash_ = 0;
  static bool RevalidateEnabled();
  uint64_t HashGuestRegions(uint32_t base, uint32_t bytes, uint32_t mip_base,
                            uint32_t mip_bytes) const;
  // The two halves of HashGuestRegions, so an upload can hash each region
  // before it reads it. HashGuestBase sets *ok false when the base does not
  // translate; HashGuestMips seeds with its result and returns the final hash.
  uint64_t HashGuestBase(uint32_t base, uint32_t bytes, bool* ok) const;
  uint64_t HashGuestMips(uint64_t base_hash, uint32_t mip_base,
                         uint32_t mip_bytes) const;
  void ParkEvictedEntry(uint64_t key, const Entry& entry);
  void DropParked(uint64_t key);
  // VerifyCachedAgainstGuest: per-host-page hashes from the previous scan.
  struct StaleVerifyRecord {
    void* texture;
    std::vector<uint64_t> page_hashes;
  };
  std::unordered_map<uint64_t, StaleVerifyRecord> stale_verify_;
  uint64_t stale_verify_hits_ = 0;
  // Census (XE_METAL_UNTILE_TIMING=1): revalidation outcomes since the last
  // report and the watch ranges that evicted entries
  // (base<<32|len -> calls, evicted).
  uint32_t census_revalidated_ = 0;
  uint32_t census_revalidate_changed_ = 0;
  uint64_t census_revalidate_ns_ = 0;
  // Signed-twin aliases served instead of a second untile (see
  // GetTextureSlotUnswizzledNull).
  uint32_t census_signed_twin_ = 0;
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> inval_ranges_;
  std::vector<void*> active_textures_;

  uint32_t draw_resolution_scale_x_ = 1;
  uint32_t draw_resolution_scale_y_ = 1;
  bool IsDrawResolutionScaled() const {
    return draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1;
  }

  struct ResolvedTarget {
    void* texture;  // id<MTLTexture>, owned by the render-target cache.
    uint32_t width;
    uint32_t height;
    uint32_t slot;
    // Exact normalized guest destination extent of the resolve represented by
    // this snapshot. Used to retire the bridge when a later independent RAM
    // writer overlaps any byte, including a partial/aliased write.
    uint32_t guest_extent_start;
    uint32_t guest_extent_length;
    // Stage 3: DEPTH bridges and non-RAM-backed color bridges are served to
    // fetches; RAM-backed color reads guest RAM (exact-address placement).
    bool is_depth;
    // True when the snapshot contains guest resolve-destination channel
    // representation, so sampling it must apply the texture fetch swizzle just
    // like the RAM upload path. Legacy resolve snapshots are already logical
    // host color and must not be decoded a second time.
    bool requires_fetch_swizzle;
    // True once any resolve at this base enqueued a guest-RAM writeback: the
    // writer could encode the dest, so RAM (not the per-base snapshot) is
    // the truth. Stays false for dests the writer skips (64bpp HDR etc.),
    // which keep the bridge (Sonic's tonemap chain went flat gray without
    // this fallback under default-ON stage 3).
    bool ram_backed = false;
    // Cache-clear classification ONLY (implement-metal-cache-clearing): does
    // the CURRENT content generation's data exist in guest RAM?
    //
    // Distinct from ram_backed, which is sticky for the life of the base and
    // drives the serve path, and reset on a BROADER condition than full_rect:
    // every successful RegisterResolvedTarget clears this, whereas full_rect
    // resets only when the texture or extent changes. The conditions differ on
    // purpose. A resolve reuses the existing snapshot object whenever it is
    // large enough, so ResolveColorToSnapshot can overwrite the content in
    // place with the pointer and dimensions unchanged -- still a new
    // generation, and registration happens before that resolve's writeback
    // decision.
    //
    // Without this, a base whose earlier resolve wrote back and whose later
    // resolve skipped or failed its writeback would still read
    // ram_backed == true, and a clear would release a snapshot holding content
    // guest RAM never received.
    bool generation_ram_backed = false;
    // XE_METAL_TIP_SNAP_VIEW: derived 1x1 copy of the snapshot's texel (0,0),
    // blitted at resolve time by the command processor (which owns the
    // texture). A 1x1 guest fetch must read guest texel (0,0), but binding
    // the full WxH snapshot puts the shader's UV mid-texture (8x8 tip ->
    // texel 4,4) — the JC2 exposure-darkness mechanism.
    void* tip_texture = nullptr;  // id<MTLTexture>, owned by the CP.
    uint32_t tip_slot = 0;
    // Exact-cover reader: true while the NEWEST guest-RAM writeback at this
    // base wrote the full registered extent at offset (0,0) with
    // pitch == width. RAM then holds scatter(snapshot) — any channel swap
    // is baked into the snapshot by the resolve shader itself, and equal
    // fetch/writeback endian makes untile cancel the scatter — so for a
    // fetch of the same extent and pitch the snapshot IS the RAM upload's
    // content and can be served directly. Offset/partial resolves (SF4's
    // column writes) clear this and keep the exact-address RAM path.
    bool full_rect = false;
    uint32_t wb_endian = 0;
    // Immutable properties of `texture` (an MTLTexture never changes size or
    // format), cached wherever `texture` is assigned: the per-binding serve
    // classification read them through Objective-C on every draw (Burnout:
    // ~15k bindings a frame).
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    uint32_t texture_pixel_format = 0;  // MTLPixelFormat
    // Draw resolution scaling only (scaled-cover serving): guest rows from the
    // top of the snapshot written by resolves (the base resolve and joined
    // strips) since the texture was created, and the union of their guest
    // extents, which retires the registration on any overlapping writer.
    uint32_t scaled_rows_written = 0;
    uint32_t scaled_extent_start = 0;
    uint32_t scaled_extent_length = 0;
    // Per-swizzle sampled views for the exact-cover fast path. Serving the raw
    // snapshot slot (rt.slot) bypasses the fetch swizzle the ordinary RAM
    // upload bakes into a texture view, so a nonidentity guest swizzle would
    // read the wrong channels. Identity RGBA reuses the raw slot; every other
    // 12-bit guest swizzle gets its own channel-swizzled MTLTexture view in a
    // fresh bindless slot, cached here. The views are ours (retained +1);
    // retired wholesale (slot -> stale_slots_, view -> retired_textures_) when
    // the snapshot texture generation changes, and released at destruction.
    struct SwizzleView {
      void* view;  // id<MTLTexture>, retained by us.
      uint32_t slot;
    };
    std::unordered_map<uint32_t, SwizzleView> swizzle_views;
    // Per-swizzle sampled views of the derived 1x1 tip_texture (bridge 1x1
    // fetch path). Kept separate from swizzle_views because they alias a
    // DIFFERENT source object with its own lifetime: they retire when the tip
    // texture generation changes (RegisterResolvedTipView), not when the full
    // snapshot changes.
    std::unordered_map<uint32_t, SwizzleView> tip_swizzle_views;
    // Content generation: bumped by every registration (each resolve to the
    // base). The fitted view below records the generation it was blitted
    // from so a stale copy is never served.
    uint32_t generation = 0;
    // Snapshot fit (see PrepareSnapshotFit): fetch-sized derived texture,
    // ours (retained +1), one per base; re-created when the fetch dims or
    // the snapshot pixel format change, re-blitted when the snapshot object
    // or generation changes.
    struct FittedView {
      void* texture = nullptr;  // id<MTLTexture>, retained by us.
      uint32_t slot = 0;
      uint32_t width = 0;   // guest fetch dims (unscaled)
      uint32_t height = 0;
      void* source = nullptr;  // snapshot object the content came from
      uint32_t source_generation = 0;
      std::unordered_map<uint32_t, SwizzleView> swizzle_views;
    };
    FittedView fitted;
    // DepthFetchIsComposite memo: the answer for one fetch shape, valid while
    // the registry is unchanged.
    mutable uint64_t composite_epoch = UINT64_MAX;
    mutable uint32_t composite_width = 0;
    mutable uint32_t composite_height = 0;
    mutable uint32_t composite_pitch = 0;
    mutable bool composite = false;
  };
  // Keyed by guest resolve destination base (fetch.base_address << 12).
  std::unordered_map<uint32_t, ResolvedTarget> resolved_targets_;
  uint64_t registry_epoch_ = 0;
  uint64_t binding_generation_ = 0;
  void RetireSlot(uint64_t submission, uint32_t slot) {
    ++binding_generation_;
    stale_slots_.emplace_back(submission, slot);
  }
  static void CacheSnapshotProperties(ResolvedTarget& rt);
  void SnapshotObjectDims(const ResolvedTarget& rt, uint32_t* width_out,
                          uint32_t* height_out) const;
  bool SnapshotFitApplies(const ResolvedTarget& rt, uint32_t fetch_w,
                          uint32_t fetch_h, bool covered_r32f) const;
  // True when `rt` is a depth snapshot smaller than the fetch and another
  // resolve's snapshot lies in the rest of the fetch's guest extent: the
  // fetch is a composition this snapshot cannot represent.
  bool DepthFetchIsComposite(const ResolvedTarget& rt, uint32_t base,
                             uint32_t fetch_w, uint32_t fetch_h,
                             uint32_t fetch_pitch) const;
  bool EnsureFittedSnapshot(ResolvedTarget& rt, uint32_t fetch_w,
                            uint32_t fetch_h);
  uint32_t ServeFittedSnapshot(ResolvedTarget& rt, uint32_t fetch_w,
                               uint32_t fetch_h, uint32_t guest_swizzle,
                               bool identity);
  void RetireFittedView(ResolvedTarget::FittedView& fitted);
  std::function<bool(void*, void*, uint32_t, uint32_t)> snapshot_fit_blit_;
  std::function<bool(void*, uint32_t, uint32_t)> snapshot_zero_;
  // Draw resolution scaling: column assemblies (see RegisterScaledAssembly),
  // keyed by the destination's x = 0 base. The texture is owned by the
  // render-target cache; the slot and swizzle views are ours.
  struct ScaledAssembly {
    void* texture = nullptr;
    uint32_t slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Contiguous guest columns from x = 0 written by column resolves since the
    // texture was created.
    uint32_t covered_width = 0;
    uint32_t extent_start = 0;
    uint32_t extent_length = 0;
    uint32_t wb_endian = 0;
    std::unordered_map<uint32_t, ResolvedTarget::SwizzleView> swizzle_views;
  };
  std::unordered_map<uint32_t, ScaledAssembly> scaled_assemblies_;
  void RetireScaledAssembly(ScaledAssembly& assembly);

  // Retires descriptors and cache-owned views without releasing the snapshot
  // or tip texture objects, whose ownership remains outside this cache.
  void RetireResolvedTarget(ResolvedTarget& target);

  // Returns the bindless slot for a channel-swizzled sampled view of `source`
  // (a resolved snapshot or its 1x1 tip), creating and caching it in `views`
  // keyed by the 12-bit guest swizzle, and marks it active for residency. The
  // caller must have already checked the swizzle is non-identity. Returns 0 on
  // view creation or heap exhaustion — the caller then picks the safe fallback
  // (RAM re-upload for RAM-backed color, null for bridge-only sources) rather
  // than bind the unswizzled raw slot with the wrong channels.
  uint32_t GetSwizzledSnapshotSlot(
      void* source, uint32_t guest_swizzle,
      std::unordered_map<uint32_t, ResolvedTarget::SwizzleView>& views);

  // The serve chain behind GetTextureSlot, which returns 0 for every fetch it
  // cannot serve without applying the fetch constant's swizzle.
  uint32_t GetTextureSlotUnswizzledNull(
      const xenos::TextureFetch& fetch, bool is_signed);
  // Views of the null texture with the fetch swizzle's constant-one channels
  // applied, keyed by the normalized swizzle (constant channels only). They
  // hold no guest content, so they live as long as the cache.
  std::unordered_map<uint32_t, ResolvedTarget::SwizzleView>
      null_swizzle_views_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
