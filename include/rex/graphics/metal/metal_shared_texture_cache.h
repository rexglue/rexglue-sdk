/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_TEXTURE_CACHE_H_
#define XENIA_GPU_METAL_METAL_SHARED_TEXTURE_CACHE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/xenos/xenos_settings.h"
#include <rex/xenos/register.h>
#include "rex/graphics/metal/metal_load_shader_pipelines.h"
#include "rex/graphics/metal/metal_mip_level_plan.h"
#include "rex/graphics/metal/metal_shared_texture_format_table.h"
#include "rex/graphics/metal/metal_texture_load_eligibility.h"
#include <gpu/xenos/pipeline/texture/cache.h>
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
class TraceWriter;
namespace metal {

class MetalSharedMemory;
class MetalTextureCache;

// C1T-R (OpenSpec implement-metal-gpu-texture-loads): the real
// rex::graphics::TextureCache subclass serving ONLY strict tiled 2D k_8_8_8_8
// kRamContent cache misses -- the single-mip class and its mipped sibling
// (OpenSpec 6.1: a chain at its own guest pointer, loaded through level-scoped
// views) -- via the proven direct-to-tiled compute load
// (texture_load_32bpb_direct). Those two classes are mutually exclusive and
// each is canonical for its own shape. The legacy MetalTextureCache remains the
// universal fallback and the single bindless-slot authority (this class
// allocates/retires slots and registers residency through its narrow
// external-slot adapter). Seam contract:
// docs/dev/archive/evidence/metal_texture_cache_integration_adr.md
// "C1T-R Phase 0.2 seam audit" —
// classification stays ClassifyResolvedFetch + the eligibility header;
// preparation runs at the pre-encoder post-overlap/drain seam on the
// existing frame command buffer; serving is keyed by fetch-constant index
// against the per-draw eligible mask the command processor computed, so
// prep-time and serve-time can never disagree.
class MetalSharedTextureCache : public TextureCache {
 public:
  // Ceilings in MiB for the host texture memory this cache holds; 0 means no ceiling.
  void SetHostMemoryLimitsMB(uint32_t soft_mb, uint32_t hard_mb) {
    host_memory_limit_soft_mb_ = soft_mb;
    host_memory_limit_hard_mb_ = hard_mb;
  }
  // Loads this cache began for a range the guest wrote before the copy completed.
  uint32_t loads_raced_by_guest_write() const { return loads_raced_by_guest_write_; }
  // Bumped by every change that can alter GetServedSlot's answer for an
  // unchanged fetch constant: loads, binding updates, views, slot releases,
  // a new submission, faults, and any prep that left a binding outdated.
  uint64_t serve_generation() const { return serve_generation_; }
  // Fault attribution: the last eight loads encoded (base page, extent,
  // levels, side/frame buffer, submission), newest last.
  struct RecentLoad {
    uint32_t base_page = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    uint64_t submission = 0;
    bool side = false;
    bool direct = false;
  };
  void NoteRecentLoad(const RecentLoad& load) {
    recent_loads_[recent_load_next_++ % 8] = load;
  }
  std::string DescribeRecentLoads() const;
  MetalSharedTextureCache(const RegisterFile& register_file, const ::rex::gpu::XenosSettings& settings,
                          MetalSharedMemory& shared_memory, void* device,
                          MetalTextureCache* legacy_adapter,
                          uint32_t draw_resolution_scale_x,
                          uint32_t draw_resolution_scale_y);
  ~MetalSharedTextureCache() override;

  // Builds the direct-load pipeline. Returns false (route unusable, callers
  // must not create the instance's route) on any failure.
  bool Initialize();

  // Command-processor wiring (all GPU-thread).
  // Returns the frame id<MTLCommandBuffer> at a pass boundary (the existing
  // PrepareTransferCommandBuffer), or, with `side`, the submission's side
  // command buffer (committed ahead of the frame one, no pass boundary), or
  // nullptr.
  void SetPrepareTransferCallback(std::function<void*(bool side)> callback) {
    prepare_transfer_ = std::move(callback);
  }
  // Whether a render encoder is open right now (a frame-buffer load would end
  // it); side loads are only worth their command buffer when it is.
  void SetRenderPassOpenQuery(std::function<bool()> query) {
    render_pass_open_ = std::move(query);
  }
  // The submission id the currently-open frame command buffer will get.
  void SetSubmissionProvider(std::function<uint64_t()> provider) {
    submission_provider_ = std::move(provider);
  }
  // True if the given submission faulted (lost command buffer): content
  // produced by GPU work in it is garbage and must not be served (M4).
  void SetSubmissionFaultQuery(std::function<bool(uint64_t)> query) {
    submission_fault_query_ = std::move(query);
  }
  // Newest GPU-completed submission; gates scratch-ring reuse for the
  // buffer-writing load families (stage-1 widening).
  // Trace playback: replays restore all-zero mip pages, so the routed load
  // declines mipped textures whose mip region is all zero (the legacy cache's
  // playback-only guard). Live RAM is authoritative and may also be simply
  // unpublished under demand-driven publication, so the probe never runs live.
  void SetTracePlayback() { trace_playback_ = true; }

  void SetCompletedSubmissionProvider(std::function<uint64_t()> provider) {
    completed_submission_provider_ = std::move(provider);
  }

  // Pre-encoder prepass (after the overlap commit/wait/drain gateway, before
  // BeginFrameEncoder): forces re-parse of the masked fetch constants (this
  // backend has no register-write notification plumbing; per-draw re-parse
  // matches the legacy cache's cost class and the base key comparison
  // prevents redundant loads) and runs the base RequestTextures flow, whose
  // LoadTextureDataFromResidentMemoryBackend encodes the direct dispatch on the
  // frame command buffer.
  // dirty_mask: the fetch constants written since the last prep (bit per
  // index); only those are re-parsed, the rest keep their bindings.
  void PrepareEligible(uint32_t eligible_mask, uint32_t dirty_mask);

  // Loads served through the buffer-writing families since creation
  // (engagement evidence for the stage-1 widening).
  uint64_t family_load_count() const { return family_load_count_; }

  // Serve at descriptor-marshal time: the bindless slot for the prepared
  // fetch-constant index, or 0 when unserved (caller falls back to the
  // legacy path). On success marks the texture used and registers encoder
  // residency through the legacy adapter.
  //
  // is_signed: serve the RGBA8Snorm view of the SAME loaded bytes (Xenos
  // 8-bit signed texels are two's-complement, i.e. exactly host snorm
  // bytes — the same equivalence the legacy plan's host_format_signed
  // uses). The view + its slot are created lazily on first signed serve of
  // a binding and retired with the binding slot.
  uint32_t GetServedSlot(uint32_t fetch_constant_index,
                         bool is_signed = false);
  // Holds GetServedSlot's nonzero answer for this binding until the next
  // prep: called by the draw's route verification for each binding it found
  // servable, so the bind that follows serves the same content even when a
  // CPU write invalidates the texture in between
  // (XE_METAL_SERVE_LATCH=0 disables).
  void LatchServe(uint32_t fetch_constant_index);
  // Diagnostic: why GetServedSlot returned 0 for this binding (static
  // string; "served" when it would not have).
  const char* UnservedReason(uint32_t fetch_constant_index, bool is_signed);
  // Diagnostic: the state GetServedSlot's answer for this binding rests on
  // (the reason, whether prep found it current, and the submissions of its
  // loads against the one being encoded).
  std::string DescribeServeState(uint32_t fetch_constant_index,
                                 bool is_signed);

  // Resolve forwarding (XE_METAL_RESOLVE_FORWARD=0 disables): a GPU-resident
  // resolve into one of this cache's single-level tiled 2D textures (same
  // pitch and texel size, starting a whole number of 32-row tile bands into
  // it) writes that texture in its scatter dispatch too, and the texture is
  // then marked current instead of reloading from the buffer (GTA IV: 36 such
  // reloads, ~75 MB, a frame). A resolve covering only part of the texture
  // (GTA IV resolves its 1280x720 targets in three strips) is forwarded only
  // into a texture that was current, whose other texels still match memory.
  // Any later write to its range fires its watch again and it reloads as
  // before.
  struct ResolveForwardTarget {
    void* texture = nullptr;      // this cache's texture
    void* mtl_texture = nullptr;  // id<MTLTexture>, an unsigned view of it
    uint32_t kind = 0;            // xe_resolve_forward_store's kind
    uint32_t endian = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t origin_y = 0;  // texture row of the destination's row 0
  };
  bool FindResolveForward(uint32_t dest_base, uint32_t dest_pitch,
                          uint32_t bpp_log2, uint32_t offset_x,
                          uint32_t offset_y, uint32_t width, uint32_t height,
                          uint32_t extent_bytes, ResolveForwardTarget* out);
  // After the resolve's own watch invalidation.
  void CompleteResolveForward(const ResolveForwardTarget& target);

  // M4 (mandatory): called by the command processor when fault publication
  // records lost submissions. Every texture whose last load rode one of
  // them is invalidated THROUGH the shared-memory authority (watch fires,
  // texture goes outdated, pages revalidate from intact RAM, next use
  // reloads) — eager and independent of the bounded faulted-submission set
  // the serve-time query consults as defense in depth.
  void OnSubmissionsFaulted(const std::vector<uint64_t>& submissions);

  // Trace capture (M5, revised by gate-C evidence): the route DISENGAGES for
  // the captured frame (the command processor's prepass checks
  // trace_writer_.is_open()), so the legacy CPU path serves and records the
  // exact CPU-oracle read-set during the frame. The one remaining
  // contribution is this warm sweep — the exact analog of the legacy
  // warm-entry sweep for textures the subclass owns: it MUST run BEFORE
  // MetalSharedMemory::WriteTraceInitialState (whose page invalidation
  // fires the watches and outdates everything) and records only textures
  // that were up-to-date at that point, with the legacy extent expression.
  void WriteAllCachedTexturesToTrace(TraceWriter& trace_writer);

  // Residency sweep companion of MetalTextureCache::AppendAllTextures:
  // every texture a live subclass-bound slot can reach.
  void AppendAllTextures(std::vector<void*>& out) const;

  // Cache-clear release (implement-metal-cache-clearing step 5).
  //
  // The inherited TextureCache::ClearCache is NOT sufficient here, which is
  // why this override exists. The base clear resets the bindings and calls
  // UpdateTextureBindingsBackend, whose implementation PARKS rather than releases
  // a binding whose texture is gone -- correct for the per-swap BeginFrame
  // reset it was written for -- and then destroys the textures. That would
  // leave every slot holding a dangling Texture*, a retained view over a
  // destroyed base texture, and a legacy bindless slot still bound to it.
  //
  // ORDERING: this releases slots and views back into the legacy cache's
  // submission-gated pools, so it MUST run before MetalTextureCache::ClearCache
  // drains those pools. Reversing the two would strand the retirements in a
  // pool that was already emptied.
  void ClearCache() override;
  // Per-swap: the base binding reset, plus the load census report
  // (MetalSharedTexCensus, every 128 frames): loads, reloads, bytes and hash
  // revalidations per frame, by format. Evidence for the pass-split cost of
  // mid-frame loads (every load ends the open render pass).
  void BeginFrame() override;
  // The base memory-limit LRU, then the descriptor-slot budget: the host
  // textures of this cache hold view slots in the legacy cache's bindless
  // heap (one per swizzle/sign view) that the memory limits know nothing
  // about. A streaming title (GTA IV) fills the heap with live, small, never
  // destroyed textures; the legacy path then has no slot for anything and
  // untiles every binding every draw (42 ms/frame). When the heap's headroom
  // is gone and this cache holds more than half of it, the least recently
  // used textures are destroyed down to 3/8 of the heap.
  void CompletedSubmissionUpdated(uint64_t completed_submission_index) override;

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override {
    // k_8_8_8_8 signed is a view over the same bytes; signed bindings are
    // route-ineligible anyway.
    return false;
  }
  uint32_t GetHostFormatSwizzle(TextureKey key) const override {
    const SharedTextureFormatEntry* entry = FindSharedTextureFormat(key.format);
    return entry ? entry->host_swizzle : xenos::REXGPU_SWIZZLE_RGBA;
  }
  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override {
    // Plain and stacked 2D (Metal's 2D array limit); cube maps six faces
    // (XE_METAL_TEX_ROUTE_CUBE); everything else, and stacked textures under
    // XE_METAL_TEX_ROUTE_STACKED=0, single-slice.
    if (dimension == xenos::DataDimension::kCube) {
      return SharedRouteCubeEnabled() ? 6u : 1u;
    }
    if (dimension == xenos::DataDimension::k3D) {
      return SharedRouteVolumeEnabled() ? 2048u : 1u;
    }
    return dimension == xenos::DataDimension::k2DOrStacked &&
                   SharedRouteStackedEnabled()
               ? 2048u
               : 1u;
  }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryBackend(Texture& texture, bool load_base,
                                               bool load_mips);
  // Upstream's pure virtual name; the backend implementation above does the work.
  bool LoadHostTextureDataFromResidentMemory(Texture& texture, bool load_base,
                                             bool load_mips) override {
    return LoadTextureDataFromResidentMemoryBackend(texture, load_base, load_mips);
  }
  void UpdateTextureBindingsBackend(uint32_t fetch_constant_mask);

 private:
  uint32_t host_memory_limit_soft_mb_ = 0;
  uint32_t host_memory_limit_hard_mb_ = 0;
  uint32_t loads_raced_by_guest_write_ = 0;
  // Dedicated-VRAM GPUs: texture memory limits sized from the working set.
  void SizeHostMemoryLimitsForDevice();
  class MetalTexture;

  // Per-fetch-constant slot state mirroring the base texture_bindings_:
  // which texture+swizzle the bound descriptor currently expresses.
  struct BindingSlot {
    const Texture* texture = nullptr;
    uint32_t host_swizzle = 0;
    void* sampled_view = nullptr;  // id<MTLTexture> (2DArray view), retained.
    uint32_t slot = 0;             // 0 = unbound.
    // Lazy RGBA8Snorm view of the same texture for SIGNED bindings.
    void* signed_view = nullptr;   // retained id<MTLTexture>, or null.
    uint32_t signed_slot = 0;      // 0 = not yet created.
    // True when this binding owns its views and slots (the per-binding mode,
    // XE_METAL_ROUTE_VIEW_CACHE=0); otherwise they belong to the texture's
    // view cache and the binding only refers to them.
    bool owns_views = false;
    // The texture was up to date when this draw's prep (PrepareEligible ->
    // RequestTextures) finished: a watch firing between prep and serve marks
    // it for the NEXT request, and the content loaded at prep is what the
    // D3D12 cache binds too. Set per prep, read by GetServedSlot.
    bool prep_current = false;
    // The draw being issued verified this binding servable (LatchServe):
    // an invalidation between that check and the bind must not change the
    // answer, because the draw has already elided its RAM convergence on
    // the strength of it. Cleared by the next prep.
    bool serve_latched = false;
  };
  void ReleaseBindingSlot(BindingSlot& binding_slot);
  // A 2DArray view (full mip range, host swizzle) of `texture` in a bindless
  // slot: from the texture's view cache, or created into it. In the
  // per-binding mode the view is created unowned and the caller owns it.
  bool AcquireView(MetalTexture& texture, uint32_t host_swizzle,
                   bool is_signed, void** view_out, uint32_t* slot_out);
  // Forgets every binding slot that refers to `texture` (it is being
  // destroyed; its cached views are retired with it).
  void ForgetTextureBindings(const Texture* texture);

  void* device_;                        // id<MTLDevice>, not owned.
  MetalTextureCache* legacy_;           // slot/residency adapter, not owned.
  void* direct_pipeline_ = nullptr;     // id<MTLComputePipelineState>, retained.
  // The compiled texture-load family (buffer-writing shaders) for every
  // non-direct table format; empty when the widening is rolled back or the
  // pipelines failed to build (those formats then stay legacy).
  MetalLoadShaderPipelines family_pipelines_;
  std::function<void*(bool side)> prepare_transfer_;
  std::function<bool()> render_pass_open_;
  // Set per load (see LoadTextureDataFromResidentMemoryBackend): the load's
  // encoders go to the side command buffer instead of ending the render pass.
  bool side_load_ = false;
  uint64_t census_side_loads_ = 0;  // loads that took the side command buffer
  uint64_t census_forwarded_ = 0;   // textures written by their resolve
  static uint32_t ResolveForwardKind(const TextureKey& key);
  // Forwardable textures by base address (see FindResolveForward).
  std::multimap<uint32_t, MetalTexture*> forward_textures_;
  RecentLoad recent_loads_[8] = {};
  uint32_t recent_load_next_ = 0;
  std::function<uint64_t()> submission_provider_;
  bool trace_playback_ = false;
  std::function<bool(uint64_t)> submission_fault_query_;
  std::function<uint64_t()> completed_submission_provider_;
  uint64_t last_begun_submission_ = 0;
  uint64_t serve_generation_ = 0;

  // Private scratch ring for the buffer-writing families: a load shader
  // untiles into a linear scratch region, then a blit copies it into the
  // host texture level in the same command buffer. Regions are bump-allocated
  // per submission and a buffer is reused only once its submission has
  // completed on the GPU.
  struct ScratchBuffer {
    void* buffer = nullptr;  // id<MTLBuffer>, private storage, retained.
    uint32_t size = 0;
    uint32_t head = 0;
    uint64_t submission = 0;
  };
  static constexpr uint32_t kScratchBufferSize = 32u * 1024u * 1024u;
  static constexpr size_t kScratchBufferMax = 8;
  std::vector<ScratchBuffer> scratch_;
  uint64_t family_load_count_ = 0;

  // Hash revalidation of a base-region reload (XE_METAL_SHARED_REVALIDATE=0
  // disables): a CPU write watch fires on every write into a texture's
  // pages, including rewrites of identical bytes (streamed atlases, UI,
  // double-buffered uploads), and every reload here ends the open render
  // pass. When the previous load's XXH3 of the guest base bytes still
  // matches for every region the reload would read (base and/or mips, hashed
  // separately), the texture is marked up to date without a load. Never
  // applied over GPU-written pages (RAM is stale there by definition), and it
  // stops hashing a texture whose content keeps changing. RDR 2026-10-16:
  // base-only coverage left 47 mip-chain reloads a frame (streamed DXT whose
  // watch ranges fire on neighbouring writes), each ending the render pass.
  static constexpr uint32_t kRevalidateMaxBytes = 8u * 1024u * 1024u;
  uint64_t HashGuestBase(uint32_t base, uint32_t bytes) const;
  // Returns true when the reload can be skipped (content unchanged).
  bool RevalidateRegions(MetalTexture& texture, bool load_base, uint32_t base,
                         uint32_t base_bytes, bool load_mips, uint32_t mips,
                         uint32_t mips_bytes);
  // Hashes RevalidateRegions computed for the load now in progress; adopted
  // by the texture only once that load succeeds. 0 = none.
  uint64_t pending_base_hash_ = 0;
  uint64_t pending_mips_hash_ = 0;

  // Load census, per swap (see BeginFrame).
  uint32_t census_frames_ = 0;
  uint64_t census_loads_base_ = 0;
  uint64_t census_loads_mips_ = 0;
  uint64_t census_reloads_ = 0;
  uint64_t census_load_bytes_ = 0;
  uint64_t census_revalidated_ = 0;
  uint64_t census_revalidate_changed_ = 0;
  uint64_t census_revalidate_gpu_skipped_ = 0;
  uint64_t census_revalidate_ns_ = 0;
  uint64_t census_destroyed_ = 0;     // textures destroyed in the window
  uint64_t census_served_stale_ = 0;  // served outdated, loaded this submission
  // Whether every outdated region of `texture` was loaded by `submission`
  // (so the binding holds this submission's prep-time content; see
  // GetServedSlot).
  static bool ServedByCurrentSubmission(const MetalTexture& texture,
                                        uint64_t submission);
  int64_t census_host_bytes_ = 0;     // live host bytes booked by this cache
  uint32_t live_textures_ = 0;        // MetalTexture objects alive
  // Bindless view-heap slots this cache holds (texture view caches and
  // binding slots), for the slot budget in CompletedSubmissionUpdated.
  uint32_t external_slots_held_ = 0;
  uint64_t census_slot_destroyed_ = 0;  // destroyed by the slot budget
  // (format | linear << 8) -> loads in the window.
  std::unordered_map<uint32_t, uint32_t> census_loads_by_format_;
  bool AllocateScratch(uint32_t size, uint32_t alignment, uint64_t submission,
                       void** buffer_out, uint32_t* offset_out);
  bool LoadLevelsDirect(Texture& texture, const std::vector<MipLevelPlan>& plan,
                        bool mipped);
  bool LoadLevelsFamily(Texture& texture, const SharedTextureFormatEntry& entry,
                        const std::vector<MipLevelPlan>& plan);

  BindingSlot binding_slots_[xenos::kTextureFetchConstantCount];

  // One planned level of a mipped load. Derived entirely from
  // GetGuestTextureLayout + GetPackedMipOffset -- no re-implemented tiled,
  // packing or endian math.
  // Thin adapter over the single planning authority in
  // metal_mip_level_plan.h (BuildMipLevelPlan), which conformance tests
  // directly. Returns false -> atomic legacy fallback, before any encoding.
  bool PlanLevels(const Texture& texture, bool load_base, bool load_mips,
                  std::vector<MipLevelPlan>* out);
  // Per-shape memo of the planner's exact-range scan. Without it EVERY load --
  // including the single-mip class -- re-runs an O(w/4 * h) Tiled2D scan
  // (~0.53 ms at 1280x720), so this preserves the cost of both classes.
  MipLevelRangeMemo range_memo_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SHARED_TEXTURE_CACHE_H_
