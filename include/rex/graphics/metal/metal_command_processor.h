/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
#define XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_

#include <array>
#include <map>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rex/graphics/shader_interpreter.h"
#include <gpu/xenos/command_processor.h>
#include "rex/graphics/host_tessellator.h"
#include "rex/graphics/metal/metal_flat_index_map.h"
#include "gpu/xenos/xenos_graphics_driver_base.h"
#include "rex/graphics/metal/metal_pass_census.h"
#include "rex/graphics/metal/metal_auto_trace_trigger.h"
#include "rex/graphics/metal/metal_draw_skip_accounting.h"
#include "rex/graphics/metal/metal_fault_episode.h"
#include "rex/graphics/metal/metal_gamma_ramp_apply.h"
#include "rex/graphics/metal/metal_memexport_gap_report.h"
#include "rex/graphics/metal/metal_memory_authority.h"
#include "rex/graphics/metal/metal_need_wait_classify.h"
#include "rex/graphics/metal/metal_resolve_shader_pipelines.h"
#include "rex/graphics/metal/metal_seam_timing.h"
#include "rex/graphics/metal/metal_zero_copy_report.h"
#include "rex/graphics/metal/metal_zpd_query_admission.h"
#include "rex/graphics/metal/metal_zpd_visibility_pool.h"
#include <rex/xenos/state.h>
#include "rex/graphics/metal/metal_fetch_static_info.h"

// Upstream's CommandProcessor no longer carries an occlusion-query API or a Memory access enum; the ZPD query path and the
// lazy-publication callbacks are the port's own, so their types live here.
enum class QueryOpenResult { kOpened, kDeferred, kPoolExhausted, kFailed };
struct ReportHandle {
  uint32_t index = 0;
  explicit operator bool() const { return index != 0; }
  // The maps and comparisons below treat a handle the way the fork's numeric one was treated.
  bool operator==(const ReportHandle& other) const { return index == other.index; }
  bool operator!=(const ReportHandle& other) const { return index != other.index; }
  bool operator<(const ReportHandle& other) const { return index < other.index; }
};
// A handle that names no report; the fork spelled it as the zero value.
inline constexpr ReportHandle kInvalidReportHandle{};

namespace std {
template <>
struct hash<::ReportHandle> {
  size_t operator()(const ::ReportHandle& handle) const noexcept { return handle.index; }
};
}  // namespace std
enum class MetalPhysicalAccess { kRead, kWrite, kDiscard, kReadNoWait, kWriteUnlocked, kWriteNoWait };

namespace rex {
namespace graphics {
namespace metal {

class MetalEncodeStream;
class MetalPipelineCache;
class MetalPrimitiveProcessor;
class MetalRenderTargetCache;
class MetalSharedMemory;
class MetalSharedTextureCache;
class MetalTextureCache;
class MetalUploadArena;
struct MetalUploadAllocation;

// Metal command processor. Overrides the same pure-virtual surface as the other
// backends. The draw, copy, and shader paths are implemented -- the "Phase 2b
// skeleton / stubs for now / behaves like the null backend" description this
// comment used to carry has been false for a long time and is retired here.
//
// Known deliberate divergence from D3D12/Vulkan: the standard 256-entry
// gamma-ramp hook is overridden and applied at present, while the PWL hook only
// preserves base tracking. PWL lookup/upload/application remains an explicit
// presentation gap; the structural silent-gap audit cannot detect this semantic
// partial implementation.
// Upstream has no occlusion-query API on CommandProcessor any more; the ZPD query path is the port's own, so its types
// and entry points are declared here rather than overriding a base that no longer has them.
// QueryOpenResult and ReportHandle are declared above, once, outside the namespace so the ZPD records can name them.

// Upstream has no fence-kind enum; the port distinguishes the two fence writes itself.
enum class GuestSignalKind { kSignal, kWait };

class MetalCommandProcessor : public ::rex::gpu::CommandProcessor {
 public:
  // Tracing diagnostics read this; the old base carried it.
  uint64_t trace_draw_index_ = 0;

  // The id of the thread this command processor runs on. The SDK had a worker thread object to compare against; upstream has
  // no such member, and the two places that ask are asking exactly this, so one id is all that is needed.
  std::thread::id gpu_thread_id_{};

 public:
  // The PM4 packet chain is the Xenos core's (CommandProcessor::ExecutePacket and friends); it
  // dispatches every draw, copy and swap through the virtuals below, exactly as it does for the
  // Vulkan and D3D12 drivers. Declaring a private chain here only invented undefined virtuals.

 public:
  MetalCommandProcessor(::rex::gpu::XenosGraphicsDriverBase* driver, const TitleInfo& info,
                        IPhysicalMemory& memory, IPhysicalWriteWatch& watch,
                        IGuestInterrupts& interrupts);
  ~MetalCommandProcessor();

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length);

  // Cache-clear request (F5 / GPU menu, or a trace player rewinding to an
  // earlier command). Latches ONLY -- the request arrives on another thread's
  // call-in, potentially mid-encoder with submissions in flight, and releasing
  // there would free memory the GPU is still reading. Execution happens at the
  // frame boundary in ExecuteDeferredCacheClear. This is the D3D12 contract,
  // whose ClearCaches likewise only sets cache_clear_requested_.
  void ClearCaches() override;
  uint32_t cache_clears_executed() const {
    // Advanced on the GPU worker thread, read from the thread driving a gate.
    // Acquire pairs with the release in ExecuteDeferredCacheClear: waiting on
    // a playback event is not a synchronization edge this code may assume.
    return cache_clears_executed_.load(std::memory_order_acquire);
  }

  // Replay-boundary derived-state retirement (fix-metal-trace-replay-state-
  // reset). IMMEDIATE, all-or-nothing, no live retention rules -- the inverse
  // of ClearCaches' deferred, retention-ruled clear. Sanctioned call sites:
  // TracePlayer's frame-initial playback entry (before the trace's restore
  // commands) and conformance harnesses (which must SetTracePlaybackContext()
  // first). Fails closed -- loud log, nothing retired -- if invoked without
  // replay/test context or at a non-quiescent boundary.
  void ResetDerivedReplayState();
  uint32_t replay_resets_executed() const {
    // Same cross-thread publication contract as cache_clears_executed().
    return replay_resets_executed_.load(std::memory_order_acquire);
  }

  void RestoreEdramSnapshot(const void* snapshot);

  // Frame-dissection tooling (--capture_dir per-draw captures + --decode):
  // reconstructs the 10MB tile-major per-sample EDRAM image from the render
  // targets owning each tile range, so the existing capture/decode/diff
  // pipeline works identically for Metal and the Vulkan/AMD references.
  // Mid-frame safe: commits + waits the frame command buffer; the next draw
  // reopens it (attachments always LOAD, so content persists).
  bool DumpEdramToBuffer(std::vector<uint8_t>& edram_out);
  // --serialize_draw_range probe: commit + wait the frame command buffer
  // (same flush DumpEdramToBuffer performs, minus the readback).
  void FlushForDrawSerialization() {
    EndFrameEncoder(metal::SeamBoundaryReason::kDebugProbe);
  }
  // Headless-tool lifecycle hook (trace-dump): plain replay reaches neither
  // FinalizeSwapFrame nor a live-logging ShutdownContext, so the seam-timing
  // report is emitted from here for those runs.
  void EmitLifecycleReports(const char* site);

  // For MetalRenderTargetCache: closes any open render encoder (a transfer is
  // a pass boundary) and returns the frame id<MTLCommandBuffer> (creating it if
  // needed) so ownership-transfer blits / snapshot uploads can be encoded.

  // Diagnostics: the current trace draw index (for correlating RT-cache
  // transfer logs with manifest/capture draw indices).
  uint32_t trace_draw_index() const { return uint32_t(trace_draw_index_); }
  // Frame counter (increments at IssueSwap) — the RT cache uses it to scope
  // same-frame c2d->d2c ping-pong detection.
  uint32_t swap_count() const { return swap_count_; }
  // The submission id the currently-open frame command buffer will get when
  // committed (commits are strictly ordered on the GPU thread). Retire lists
  // (textures, bindless slots, arena regions) are tagged with it so
  // reclamation frees exactly what completed submissions can no longer read.
  uint64_t CurrentEncodingSubmission() const { return submission_counter_ + 1; }
  // XE_METAL_S7_OFF=1 live-bisect kill switch (pre-S7 lifetime semantics).
  // PROMOTED default (promote-metal-async-submission, GO 2026-08-09):
  // unset = async submission + ring arena + retirement tags; exact
  // XE_METAL_ASYNC=0 = full pre-promotion sync semantics (bit-identical
  // rollback); exact 1 = async (explicit); anything else warns once and
  // fails closed to the pre-promotion sync default. XE_METAL_SYNC_WAIT
  // retains its independent meaning (sync submission on the ring/tags
  // lifetime -- the lifetime-isolation diagnostic).
  static bool AsyncSubmissionEnabled();
  static bool S7LifetimeOff();
  static bool S7ArenaResetOff();  // XE_METAL_S7_OFF_ARENA
  static bool S7TagsOff();        // XE_METAL_S7_OFF_TAGS
  // XE_METAL_NEED_BASED_WAITS=1 (exact-1 opt-in, warn-once fail-closed,
  // DEFAULT OFF): the consumer corridor waits for the producers a request
  // actually selected instead of taking an unconditional synchronous boundary
  // (OpenSpec optimize-need-based-consumer-waits, Route A). Not a promoted
  // default and not delivered capability -- the D5 gate decides that. Under
  // XE_METAL_ASYNC=0 every record is untagged, so the feature classifies every
  // request as a fallback and is structurally inert regardless of this flag.
  static bool NeedBasedWaitsEnabled();
  // XE_METAL_SKIP_CONSUMER_WAIT=1 (exact-1 opt-in, warn-once fail-closed,
  // DEFAULT OFF): DIAGNOSTIC CEILING PROBE, never shippable. The consumer
  // corridor commits asynchronously, takes NO completion wait, and publishes
  // pending producer records without proof of finality -- consumers may read
  // STALE guest RAM by design. Exists solely to bound the fps recoverable by
  // any coherence redesign of the consumer-dependency post-wait corridor
  // (OpenSpec optimize-native-metal-frametime); an arm run under this flag is
  // performance evidence only and must never be read as a correctness arm.
  static bool SkipConsumerWaitEnabled();
  // XE_METAL_SERVING_CENSUS=1 (exact-1, default OFF): OBSERVATIONAL census of
  // the disagreement between the consumer-dependency declaration
  // (`BridgeServes`) and the serve chain's classification
  // (`ClassifyResolvedFetch`). Counts only; it never changes what is declared
  // or served. Task 1.1 of implement-metal-gpu-visible-resolve-authority.
  static bool ServingCensusEnabled();
  // XE_METAL_GPU_RESOLVE=1 (exact-1, default OFF): Stage B — canonical packed
  // resolves scatter guest-tiled into the shared buffer and become
  // GPU-current, so shared-buffer consumers need ordering rather than a
  // completion wait. Unset keeps the staging + CPU-scatter path byte-identical.
  static bool GpuResolveScatterEnabled();
  // XE_METAL_SHARED_RESOLVE=1 (exact-1, default OFF): dispatch the SHARED
  // cross-backend resolve-copy compute shaders instead of the Metal-specific
  // pack kernel, resolving EDRAM straight into the shared-memory buffer at
  // guest tiled addresses exactly as D3D12 and Vulkan do
  // (docs/dev/archive/evidence/metal_shared_resolve_port_assessment.md).
  static bool SharedResolveShadersEnabled();
  // Shared resolve shaders for array / slice destinations only
  // (XE_METAL_SHARED_RESOLVE_ARRAYS, default on).
  static bool SharedResolveArraysEnabled();
  static bool MsaaResolveOnStoreEnabled();
  // Fixed-point resolve destinations packed by copy_dest_number
  // (XE_METAL_RESOLVE_NUMBER_FORMAT, default on).
  static bool ResolveNumberFormatEnabled();

 private:
  // Shared cross-backend resolve-copy dispatch (task 4.2). Returns false when
  // unavailable; the caller keeps its existing path.
  bool DispatchSharedResolveCopy(const draw_util::ResolveInfo& resolve_info);
  MetalResolveShaderPipelines resolve_shader_pipelines_;
  uint64_t shared_resolve_dispatches_ = 0;

 public:

 private:
  // Debug probes in the RT cache flush the frame CB directly.
  friend class MetalRenderTargetCache;
  bool SetupContext() override;
  void ShutdownContext() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  ::rex::gpu::Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 ::rex::gpu::CommandProcessor::IndexBuffer* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;

  // Contract 1 gamma-ramp engagement census (temporary, default-off).
  // These overrides ONLY count raw writes; they deliberately do not classify,
  // because a partial register update is not yet a completed ramp. The base
  // class's own tracking is preserved by calling through.
  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;
  // Guest gamma-state census (XE_GAMMA_CENSUS=1). Retained audit instrument —
  // see docs/dev/metal_pwl_gamma_audit.md.
  void CensusGammaState();

  // Zero-copy lifecycle evidence, emitted from GPU-thread LIFECYCLE sites only
  // -- never from the faulting guest thread that records part of it, which may
  // not log. Takes the overlap snapshot ONCE (it is read under the physical
  // invalidation callback's lock) and hands the same one to both reporters
  // below, so enabling the cost line adds no second lock acquisition.
  void ReportZeroCopyLifecycle(const char* site);
  // Pre-gate task 5.4. Silent unless alias mode installed the observer, so
  // exact-off and every fallback arm produce no overlap line.
  void ReportZeroCopyOverlap(const char* site,
                             const metal::ZeroCopyOverlapSnapshot& snapshot);
  // Pre-gate tasks 6.3/6.4 runtime cost evidence (XE_METAL_ZERO_COPY_REPORT=1,
  // exact-off). Emitted on EVERY backing -- exact-off mirror, forced-fallback
  // mirror and alias -- because the alias arm's cost claim is only meaningful
  // against a measured control, and the control arms are exactly the runs where
  // the overlap reporter above is required to stay silent. Reads existing
  // sources (the meter, the shared memory's upload accounting, the overlap
  // snapshot, the startup observation); it computes no fact of its own and
  // gates nothing.
  void ReportZeroCopyRuntimeCost(
      const char* site, const metal::ZeroCopyOverlapSnapshot& snapshot);
  // Cadence for the reports above, applied by metal::ZeroCopyOverlapReportDue
  // to the monotonic SWAP FINALIZATION index -- every IssueSwap return path,
  // not only the ones that published. Finalization 1 is the first frame
  // boundary, so one rule covers "report immediately" and "report
  // periodically". Finalization and swap counts are carried in the lines
  // SEPARATELY from the observation counters: how often the evidence was
  // printed is not evidence.
  static constexpr uint32_t kZeroCopyOverlapSwapReportInterval = 512;

  void InitializeTrace();
  std::string GetCaptureDeviceInfoJson() const;

  // Opens the durable pipeline storage for the launched title (task group 3a).
  // The emulator already calls this on every launch; Metal simply never
  // overrode it before.
  // The base class calls the three-argument form when the runtime asks for shader storage. Overriding only the
  // four-argument convenience left this backend inheriting the base's empty implementation, so the durable caches were
  // never opened and every launch re-translated every shader.
  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking) override;
  void SaveShaderStorageToDisk();

  // Converges completed resolve writebacks and memexports to guest RAM. The
  // promoted selected executor supplies a unique authority sequence, allowing
  // ordered lookup and exact retirement without rebuilding either full pending
  // vector for every selected record. The broad range form remains available
  // for behavior-preserving internal use and never drains a disjoint record.
  //
  // `boundary_evidence` is the caller's PROOF that a record carrying no owning
  // submission tag has nevertheless been made final. Since the async promotion
  // (2026-08-09) TAGGED records are the default -- S7TagsOff() is false unless
  // XE_METAL_ASYNC=0 or an explicit S7 kill switch is set -- so the untagged
  // case this parameter exists for is now the ROLLBACK/diagnostic shape plus
  // whatever registers before a submission exists, not the ordinary one. It
  // still cannot be dropped: an untagged record's finality comes from the
  // synchronous commit-and-wait the consumer path performs, never from the
  // record. It is a leading parameter with NO default precisely so it cannot be
  // omitted: only ResolvePendingProducerDependencies can establish it, and it
  // is threaded through DrainSelectedPendingProducers to here so the
  // convergence site cannot fabricate it locally. There is exactly one caller
  // today, and that is the structure, not an accident. Under need-based waits
  // that caller passes kNone on its tagged paths by design (D1a): the shared
  // gate decides a tagged record by its tag alone, so no evidence is owed and
  // none may be invented.
  //
  // BOTH halves of this drain consult that one gate
  // (metal::DecideMemexportConvergenceGate): memexport records via
  // ConvergeCompletedMemexportRam, resolve writebacks directly. The rule is
  // about tags and evidence and never reads TransitState, so the two producer
  // kinds cannot drift apart -- an untagged record of EITHER kind is refused
  // and retained unless the caller proved a synchronous boundary.
  metal::ProducerDrainOutcome DrainPendingWritebacks(
      metal::MemexportBoundaryEvidence boundary_evidence,
      uint32_t filter_base = 0, uint32_t filter_size = 0xFFFFFFFFu,
      uint64_t filter_authority_sequence = 0);
  metal::ProducerDrainOutcome DrainSelectedPendingProducers(
      const metal::ConsumerDecision& decision,
      metal::MemexportBoundaryEvidence boundary_evidence);
  struct ProducerDependencyOutcome {
    metal::ProducerDrainOutcome drain;
    bool complete = true;
  };
  ProducerDependencyOutcome ResolvePendingProducerDependencies(
      const metal::ConsumerDecision& decision);
  // Bounds the lazily armed memexport backlog (XE_METAL_LAZY_MEMEXPORT_LIMIT):
  // above the limit, publishes the completed armed records to RAM at the swap
  // instead of leaving them for a CPU read that may never come.
  void TrimLazyMemexportBacklog();
  bool PrepareGuestMemoryForHostRead(uint32_t guest_address,
                                     uint32_t length);
  bool PrepareGuestMemoryForCompletionExposure();
  void PrepareGuestMemoryForFenceWrite(GuestSignalKind kind);
  // Closes the open command buffer when it holds a GPU-current resolve or
  // memexport record armed for CPU faults, so a fault behind the fence the
  // guest is about to see can wait for committed work. `wait` also waits for
  // the buffer to complete (completion signals); `wait_for_exports` waits
  // only when the open buffer holds a memexport record.
  void CommitOpenFaultableWork(bool wait, bool wait_for_exports = false);
  bool FlushPendingWritebacksForWait(uint32_t guest_address);

  bool ResolveRamConsumerDependencies(
      metal::ConsumerKind consumer,
      const metal::GuestRangeSet& requested_ranges);

  // Cross-writer memory-authority dependency seam (OpenSpec
  // implement-metal-cross-writer-memory-authority, tasks 2.4 and 4.1).
  // Snapshots the current pending resolve/memexport records into the
  // device-free authority vocabulary for the common dependency query.
  // Non-owning and side-effect free: it reads the existing queues, never
  // mutates them.
  void SnapshotPendingProducersInto(
      std::vector<metal::PendingProducer>& out,
      std::vector<metal::PendingProducer>& scratch) const;
  std::vector<metal::PendingProducer> SnapshotPendingProducers() const;
  metal::ConsumerDecision QueryPendingProducerDependencies(
      metal::ConsumerKind consumer,
      const metal::ConsumerRequest& request) const;

  // --- Draw path helpers (metal_command_processor.mm) ---

  // Creates the bindless heaps, the fixed system view table (shared-memory SRV),
  // the null buffer/texture/sampler, and the per-frame upload arena.
  bool InitializeDrawResources(void* device);
  void ShutdownDrawResources();

  // Returns a cached id<MTLDepthStencilState> for the given RB_DEPTHCONTROL
  // value + RB_STENCILREFMASK(_BF) masks (the reference values are encoder
  // state, not descriptor state), creating it on first use.
  void* GetOrCreateDepthStencilState(uint32_t depth_control,
                                     uint32_t stencil_ref_mask,
                                     uint32_t stencil_ref_mask_bf);

  // Opens the frame command buffer (lazily, once per frame) and a render encoder
  // into the color RT if one isn't already open. The render pass clears on the
  // first open of a frame and after each resolve (a pass boundary, see
  // reopen_clear_); otherwise it loads so multiple draws accumulate. Returns the
  // encoder (id<MTL...>) or nullptr on failure.
  void* BeginFrameEncoder();
  // Closes any open render encoder (a transfer is a pass boundary) and returns
  // the frame id<MTLCommandBuffer>, creating it if needed. `site` names the
  // caller for the pass census (metal::MetalTransferSite).
  void* PrepareTransferCommandBuffer(uint32_t site = 0);
  // Ends the current render encoder only (keeps the frame command buffer open so
  // a resolve blit / the next pass can be encoded on it). The reason is the
  // pass census attribution (metal_pass_census.h) and is deliberately
  // DEFAULT-FREE: a new pass end does not compile until it says why. `detail`
  // is the SeamBoundaryReason index for kSubmission; `resolve_touches_pass`
  // says, for the resolve reasons, whether the resolved tiles belong to one of
  // the open pass's attachments.
  void EndRenderEncoder(metal::MetalPassEndReason reason, uint32_t detail = 0,
                        bool resolve_touches_pass = false);
  // Render passes begin and end on the encoder thread (see
  // MetalEncodeStream::BeginRenderPass).
  bool DeferredPassLifecycle() const;
  // Waits for an undrained deferred pass end (pass_end_undrained_).
  void DrainUndrainedPassEnd();
  // XE_METAL_PASS_CENSUS=1 (exact-1, default OFF): observational draw-pass
  // census (reduce-metal-draw-pass-traffic task 0.1).
  static bool PassCensusEnabled();
  // Materialize overwrite skip (XE_METAL_OVERWRITE_SKIP): for a single guest
  // rectangle, the render target slots (bit 0 depth and stencil, 1 + i colour
  // i) whose every sample it writes, and the guest pixel rectangle it covers
  // (the vertex shader runs on the CPU for its 3 vertices).
  uint32_t ComputeDrawOverwrite(xenos::PrimitiveType prim_type,
                                const ::rex::gpu::Shader* vertex_shader,
                                const ::rex::gpu::Shader* pixel_shader,
                                uint32_t normalized_color_mask,
                                reg::RB_DEPTHCONTROL normalized_depth_control,
                                float rect_out[4]);
  std::unique_ptr<::rex::gpu::ShaderInterpreter> overwrite_interpreter_;
  // reduce-metal-draw-pass-traffic D2: a draw pass binds only the attachments
  // its draws can read or write (metal_pass_attachment_use.h). Config
  // ::rex::graphics::metal::GetMetalSettings().metal_pass_attachments_by_use.Get(), XE_METAL_PASS_ATTACHMENTS=0/1 override,
  // XE_METAL_PASS_ATTACHMENTS_LEGACY=1 forces the bind-everything policy.
  static bool PassAttachmentsByUseEnabled();
  // The encoder identity BeginFrameEncoder compares: the render-target cache's
  // accumulated attachment key, distinguished for scratch-depth and memexport
  // passes.
  uint64_t PassAttachmentKey(bool dummy_depth, bool memexport) const;
  // Ends any open encoder, then commits + waits on the frame command buffer.
  // The reason parameter is the seam-timing global attribution layer
  // (metal_seam_timing.h) and is deliberately DEFAULT-FREE: a new call site
  // does not compile until it declares why it closes the frame buffer.
  void EndFrameEncoder(metal::SeamBoundaryReason reason);
  // Staged hazard upload (see MetalSharedMemory::SetStagedUploadCallback):
  // copies guest RAM [offset, offset+length) into a staging buffer and
  // encodes a blit into the shared buffer on the open frame command buffer.
  uint64_t StageHazardUpload(uint32_t offset, uint32_t length);
  // Discrete shared-memory mirror (MetalSharedMemory::is_discrete_mirror):
  // copies a CPU-mirror range into the VRAM copy with a blit on the side
  // command buffer, which runs ahead of the open submission's draws.
  void EncodeDiscreteMirrorUpload(uint32_t offset, uint32_t length);
  void EndDiscreteUploadBlit();
  // At commit: blits this submission's GPU-produced ranges (writebacks,
  // memexports) from the VRAM copy back into the CPU mirror, so publication
  // and lazy reads after the submission completes see them.
  void EncodeDiscreteMirrorReadback(void* command_buffer, uint64_t submission);
  void NoteDiscreteProducedRange(uint32_t start, uint32_t length) {
    if (discrete_mirror_ && length) {
      discrete_produced_ranges_.emplace_back(start, length);
    }
  }
  // A lazily published resolve: the CPU rarely reads it, so it stays in VRAM
  // and is fetched on demand (MetalSharedMemory::EnsureCpuCurrent) unless an
  // earlier fetch made its base hot.
  void NoteDiscreteProducedResolve(uint32_t start, uint32_t length);

  // Depth-resolve bridge: runs a pass that samples the Depth32Float depth
  // texture (as depth2d) and writes its value into the R32Float snapshot
  // texture, so deferred lighting can sample the depth as texture2d_array<float>.
  // dest_y_offset places a multi-strip resolve's rows inside a full-height
  // snapshot (source rows align by frame-absolute pixel position). Builds the
  // conversion pipeline on first use. Returns false on failure.
  bool ConvertDepthToSnapshot(void* depth_texture, void* dest_snapshot,
                              uint32_t width, uint32_t height,
                              uint32_t dest_y_offset = 0);

  // The session's draw resolution scale (metal_draw_resolution_scale.h).
  uint32_t draw_resolution_scale_x_ = 1;
  uint32_t draw_resolution_scale_y_ = 1;
  // Draw resolution scaling: column assemblies by x = 0 base -> dest pitch.
  std::unordered_map<uint32_t, uint32_t> scaled_assembly_pitches_;
  // Depth column strips: x = 0 base -> dest pitch of a depth destination the
  // guest resolves as vertical columns (Viva Pinata's 2 x 640x720 strips of a
  // 1280-pitch scene depth, fetched 1280x720 from the first column's base).
  std::unordered_map<uint32_t, uint32_t> depth_column_pitches_;
  // Rows of the x = 0 column of each such destination: where a second row of
  // columns starts when the guest resolves the depth as quadrants.
  std::unordered_map<uint32_t, uint32_t> depth_column_rows_;
  std::unique_ptr<MetalPipelineCache> pipeline_cache_;
  std::unique_ptr<MetalRenderTargetCache> render_target_cache_;
  std::unique_ptr<MetalSharedMemory> shared_memory_;
  std::unique_ptr<MetalTextureCache> texture_cache_;
  // C1T-R: the real shared rex::graphics::TextureCache subclass serving strict
  // tiled RGBA8 kRamContent misses (canonical for that class); null only if
  // its pipeline initialization failed, in which case the legacy cache
  // serves everything. Destroyed before texture_cache_ (its slot/retirement
  // adapter).
  std::unique_ptr<MetalSharedTextureCache> shared_texture_cache_;
  // Per-draw eligible fetch-constant mask computed by the pre-encoder
  // prepass; descriptor marshalling serves exactly these indices from the
  // shared subclass (seam audit M2 — prep and serve share one decision).
  uint32_t draw_shared_route_mask_ = 0;
  // Submission being encoded when this draw's routed fetches were verified
  // servable (diagnostic: a later serve refusal names it).
  uint64_t draw_route_verify_submission_ = 0;
  // True while the current draw's consumer dependencies were satisfied by
  // GPU ordering (kOrderAgainstGpuProducers) instead of a wait+drain; the
  // serve site fails a routed fetch's legacy fallback closed under it.
  bool draw_gpu_ordering_active_ = false;
  // Render encoder (identity only) that holds a routed rasterizing memexport
  // producer; draws ordered against a pending export in it take an in-encoder
  // buffer barrier. Stale identities only cost a redundant barrier.
  uintptr_t raster_export_encoder_ = 0;
  // Corridor test shared by the route mask and the serve site: does this
  // fetch's guest range overlap a pending GPU-current record? Signed
  // bindings route only in that case (the snorm-view serve carries a
  // ±1-LSB drift on ordinary content; the corridor is where routing pays).
  bool FetchOverlapsPendingGpuCurrent(
      const xenos::TextureFetch& fetch);
  bool SharedRouteEligible(const xenos::TextureFetch& fetch,
                           bool is_signed, uint32_t width, uint32_t height,
                           uint32_t pitch_texels, bool* corridor_required_out);
  std::unique_ptr<MetalUploadArena> upload_arena_;
  // PP1: shared rex::graphics::PrimitiveProcessor subclass, the single normalization
  // authority for routed native non-expanded restart-disabled line/triangle
  // list/strip draws. Null only if its initialization failed, in which case
  // every draw stays on the inline path.
  std::unique_ptr<MetalPrimitiveProcessor> primitive_processor_;

  // Bindless / fixed-resource infrastructure for the MSC argument buffer. All
  // are id<...> held as retained void*.
  void* view_bindless_heap_ = nullptr;     // id<MTLBuffer> of IRDescriptorTableEntry
  void* sampler_bindless_heap_ = nullptr;  // id<MTLBuffer> of IRDescriptorTableEntry
  void* system_view_tables_ = nullptr;     // id<MTLBuffer> of IRDescriptorTableEntry
  void* null_buffer_ = nullptr;            // id<MTLBuffer> stand-in for inactive CBVs
  void* null_texture_ = nullptr;           // id<MTLTexture> 1x1, for unbound samples
  void* null_sampler_ = nullptr;           // id<MTLSamplerState>

  // Real guest samplers: filters/clamps/aniso packed into a key ->
  // sampler-heap slot, created on first use (states retained until shutdown).
  bool GetOrCreateSamplerSlot(uint64_t key, uint32_t* out_slot);
  std::unordered_map<uint64_t, uint32_t> sampler_slot_cache_;
  uint32_t next_sampler_slot_ = 1;     // 0 == null sampler.
  uint32_t sampler_heap_slots_ = 0;    // Set when the heap is created.
  // XE_METAL_SAMPLER_CENSUS: unique guest sampler tuple -> binding count.
  std::unordered_map<uint64_t, uint32_t> sampler_census_;
  std::vector<void*> sampler_states_;  // id<MTLSamplerState>, retained.

  // Cache of id<MTLDepthStencilState> keyed by RB_DEPTHCONTROL's depth+stencil
  // bits combined with the read/write masks of RB_STENCILREFMASK(_BF).
  std::unordered_map<uint64_t, void*> depth_stencil_states_;

  // Whether the device supports mesh shaders (Apple7 / Mac2 family), a
  // prerequisite for the MSC geometry-emulation path that expands
  // rectangle/point/quad lists. Set in InitializeDrawResources.
  bool mesh_shader_supported_ = false;
  // Rectangle lists expanded in the vertex shader (two-triangle strips,
  // HostVertexShaderType::kRectangleListAsTriangleStrip) instead of MSC
  // geometry emulation: GPUs other than Apple's (metal_rect_vs_expansion).
  bool rect_vs_expansion_ = false;
  // XE_METAL_RECT_VS=1: expand even for titles whose config sets
  // metal_rect_vs_title_off.
  bool rect_vs_env_forced_ = false;
  // Auto-indexed quad lists become triangle lists through the shared
  // primitive processor instead of MSC geometry emulation: the mesh-shader
  // quad expansion streaks Red Dead's foliage on AMD (RX 6900 XT, 2026-10-10).
  // Default: non-Apple GPUs. XE_METAL_QUAD_CONVERT=1/0 forces it on/off.
  bool convert_auto_quads_ = false;

  // Host tessellation route (implement-metal-host-tessellation task 3.4): the
  // D3D11 reference tessellator + pattern cache, and a reusable host-order
  // staging copy of the guest control-point index / factor stream. Created
  // lazily on the first tessellated draw so the OFF arm allocates nothing.
  std::unique_ptr<HostTessellator> host_tessellator_;
  std::vector<uint32_t> host_tessellation_stream_scratch_;

  // id<MTLRenderPipelineState> for the depth->R32Float conversion pass, built on
  // first depth resolve (see ConvertDepthToSnapshot).
  void* depth_convert_pipeline_ = nullptr;

  // Frame command buffer / render encoder (id<...>). The command buffer lives
  // for the whole frame (committed at IssueSwap); the render encoder is opened
  // and closed multiple times as resolves and render-target changes split the
  // frame into passes.
  void* frame_command_buffer_ = nullptr;
  void* frame_render_encoder_ = nullptr;
  // 4 MiB GPU-zeroed buffer for heap fitted-snapshot views (retained).
  void* snapshot_zero_buffer_ = nullptr;
  bool frame_open_ = false;  // a render encoder is currently open.
  // The open pass was begun on the encoder thread (MetalEncodeStream::
  // BeginRenderPass): frame_render_encoder_ is then a token, not an encoder.
  bool frame_encoder_deferred_ = false;
  // A deferred pass was ended without a Drain (the next pass begins on the
  // encoder thread too); the frame command buffer must not be used directly
  // until DrainUndrainedPassEnd().
  bool pass_end_undrained_ = false;
  // Set while BeginFrameEncoder ends the open pass to begin the next one.
  bool ending_for_next_pass_ = false;
  // IssueDraw's texture binding memo (XE_METAL_BIND_MEMO).
  struct BindMemo {
    struct Key {
      const void* vertex_shader;
      const void* pixel_shader;
      uint32_t route_mask, flags, scale, pending_writebacks, pending_memexports,
          pad;
      uint64_t submission, binding_generation, registry_epoch,
          serve_generation, authority_sequence;
    };
    bool valid = false;
    Key key{};
    // Fetch constants the memo's shaders read (cached per shader pair).
    const void* mask_vertex_shader = nullptr;
    const void* mask_pixel_shader = nullptr;
    uint32_t fetch_mask = 0;
    std::vector<uint32_t> indices[2];  // vertex, pixel descriptor indices
    std::vector<void*> active;         // the residency list they declared
    uint64_t hits = 0, misses = 0, verified = 0, mismatches = 0;
  } bind_memo_;
  uint32_t bind_memo_fetch_written_ = 0;
  // marshal_desc_indices builds each stage's indices here (vertex, pixel).
  std::vector<uint32_t> bind_index_scratch_[2];
  // Hot-path render-encoder calls (direct, or recorded and replayed on the
  // encoder thread; XE_METAL_ENCODE_THREAD). Drained before the command
  // processor ends or otherwise touches frame_render_encoder_ directly.
  std::unique_ptr<MetalEncodeStream> encode_stream_;
  // gpuAddress of the view/sampler heaps, system view tables and null buffer,
  // keyed by their pointers (IssueDraw's argument buffer).
  void* fixed_gpu_address_ptrs_[4] = {};
  uint64_t fixed_gpu_addresses_[4] = {};
  // Side command buffer of the open submission: first loads of CPU-sourced
  // textures by the shared texture cache, encoded without ending the render
  // pass and committed just ahead of the frame command buffer (resources are
  // hazard-tracked, so the draws order after the loads). Faults land on the
  // same submission id.
  void* side_command_buffer_ = nullptr;
  void* PrepareSideTransferCommandBuffer();
  void* EnsureSideCommandBuffer();
  bool discrete_mirror_ = false;
  void* discrete_upload_blit_ = nullptr;  // id<MTLBlitCommandEncoder>, retained
  std::vector<std::pair<uint32_t, uint32_t>> discrete_produced_ranges_;
  uint64_t discrete_upload_blits_ = 0, discrete_upload_bytes_ = 0;
  uint64_t discrete_readback_blits_ = 0, discrete_readback_bytes_ = 0;
  uint64_t discrete_deferred_ranges_ = 0;
  uint64_t direct_index_draws_ = 0;  // guest indices swapped by the VS
  // Texture heaps: heaps declared on the open encoder and for which stages.
  uint32_t encoder_heaps_declared_ = 0;
  uint32_t encoder_heap_stages_ = 0;
  uint64_t heap_declarations_ = 0, heap_skipped_declarations_ = 0;
  uint64_t texture_declarations_ = 0;
  // Fetch constants written since the shared texture cache last parsed
  // them (bit = fetch constant index). Set by WriteRegister, consumed by the
  // draw prep: a draw whose fetch constants did not change since the previous
  // draw re-parses none of them (the D3D12 cache's register-write rule; before
  // this every draw re-parsed every binding, ~5 ms a frame of RDR's 3789
  // draws). XE_METAL_FETCH_DIRTY=0 restores the per-draw re-parse.
  uint32_t fetch_constant_dirty_mask_ = ~0u;
  // Per fetch constant: whether its four component signs are all kUnsigned
  // (fetch_plain_mask_), and whether that answer is current
  // (fetch_plain_known_mask_, cleared by WriteRegister on a changed dword).
  // The pixel-shader variant certification reads these in O(bindings) bit
  // tests instead of decoding every bound fetch constant per draw.
  uint32_t fetch_plain_mask_ = 0;
  MetalFetchStaticInfo fetch_static_info_[32] = {};
  uint32_t fetch_plain_known_mask_ = 0;
  // Per pixel shader (ucode hash): the fetch constants its bindings sample.
  std::unordered_map<uint64_t, uint32_t> pixel_shader_fetch_masks_;
  void WriteRegister(uint32_t index, uint32_t value) override;
  void OnRegistersRestoredRaw(uint32_t first_register,
                              uint32_t register_count);
  void CommitSideCommandBuffer(uint64_t submission, uint64_t submit_epoch);

  // The attachment set (from MetalRenderTargetCache::current_attachment_key)
  // the open encoder was created with; a draw whose accumulated render targets
  // differ closes the encoder and opens a new pass.
  uint64_t encoder_attachment_key_ = 0;
  // The MetalPassAttachment set the open encoder binds with Load + Store
  // (depth, stencil, colour 0..3); DontCare scratch attachments are excluded.
  uint32_t encoder_bound_attachments_ = 0;
  // The attachment set the next draw's pass must bind (IssueDraw decides it
  // from the draw's use and the open pass; BeginFrameEncoder applies it when
  // by-use binding is enabled).
  uint32_t pending_pass_bound_ = metal::kMetalPassAttachmentAll;
  // Learned attachment use per pass key (D2): the OR of every draw's use ever
  // seen under that key. A pass binds the learned set, so after a key's first
  // occurrence the pass structure equals the bind-everything policy's (no
  // "attachment needed" splits, which this GPU does not render invisibly:
  // a pass boundary between blended draws moves colour LSBs) while planes the
  // key never touches stay unbound. Bounded by the number of attachment
  // configurations a title uses.
  MetalFlatIndexMap pass_learned_use_;
  // The learned-set signature of the open pass and of the pass the next draw
  // opens: the attachment key mixed with the pass's first draw's shader pair,
  // so passes of one key that differ in content (a stencil-masked UI pass and
  // a stencil-free post pass over the same targets) learn separately.
  uint64_t encoder_pass_signature_ = 0;
  uint64_t pending_pass_signature_ = 0;
  // Per-encoder redundant-state elision: the depth-stencil state and stencil
  // references last set on the open encoder (Metal's validation layer counts
  // ~2000 redundant setDepthStencilState calls per RDR frame).
  void* encoder_depth_stencil_state_ = nullptr;
  uint32_t encoder_stencil_ref_front_ = ~0u;
  uint32_t encoder_stencil_ref_back_ = ~0u;
  // Per-encoder elision of repeated useResource declarations and buffer
  // binds (XE_METAL_ENCODER_DEDUPE=0 disables). A declaration covers the
  // whole render pass, and the bindless heaps never change within one, yet
  // every draw re-declared ~15 resources and re-bound both heaps on two
  // stages (Red Dead gameplay: ~7,000 draws in ~170 passes per frame).
  // resource -> usage bits | stage bits << 8 already declared on the encoder.
  // Open-addressing table cleared per encoder by generation: no node
  // allocation, no rehash (std::unordered_map::try_emplace per resource per
  // draw was ~3% of GTA IV's command-processor thread, 2026-10-17 line
  // profile). When it fills past three quarters the caller declares without
  // deduplication for the rest of the encoder.
  struct DeclaredResourceTable {
    struct Slot {
      void* key = nullptr;
      uint32_t bits = 0;
      uint32_t generation = 0;
    };
    static constexpr size_t kSlots = 4096;  // power of two
    std::vector<Slot> slots = std::vector<Slot>(kSlots);
    uint32_t generation = 1;
    size_t count = 0;
    void Clear() {
      count = 0;
      if (++generation == 0) {
        for (Slot& slot : slots) {
          slot.generation = 0;
        }
        generation = 1;
      }
    }
    // The slot's bits for `key` (0 for a key first seen on this encoder), or
    // nullptr when the table is too full to track it.
    uint32_t* Find(void* key) {
      if (count * 4 >= kSlots * 3) {
        return nullptr;
      }
      size_t i = (uintptr_t(key) >> 4) & (kSlots - 1);
      for (;;) {
        Slot& slot = slots[i];
        if (slot.generation != generation) {
          slot.key = key;
          slot.bits = 0;
          slot.generation = generation;
          ++count;
          return &slot.bits;
        }
        if (slot.key == key) {
          return &slot.bits;
        }
        i = (i + 1) & (kSlots - 1);
      }
    }
  };
  DeclaredResourceTable encoder_declared_resources_;
  struct EncoderBufferBind {
    void* buffer = nullptr;
    uint64_t offset = 0;
  };
  // [stage kind: vertex, fragment, object, mesh][bind point slot 0..2].
  EncoderBufferBind encoder_buffer_binds_[4][3];
  // Fixed-function state IssueDraw last set on the open encoder
  // (XE_METAL_STATE_DEDUPE=0 disables). Nothing else sets these on the draw
  // encoder, and a new encoder resets them with the rest. Burnout set all of
  // it on each of ~7,600 draws a frame: ~5% of the command-processor thread
  // on the M1 Max, mostly repeats of the previous draw.
  struct EncoderFixedState {
    void* pipeline = nullptr;
    // A deferred pipeline request `pipeline` points at, kept alive so its
    // address cannot be recycled while it is the dedupe key.
    std::shared_ptr<void> pipeline_owner;
    bool viewport_valid = false, scissor_valid = false;
    bool depth_bias_valid = false, blend_color_valid = false;
    int32_t depth_clip = -1, cull = -1, winding = -1, fill = -1;
    // IssueDraw's IR draw-parameter binding (XE_METAL_DRAW_PARAMS).
    int32_t draw_params = -1;
    // Scissor clamp extent of the bound attachments (the encoder retains
    // them, so the pointers cannot be recycled while it is open).
    bool extent_valid = false;
    void* extent_key[5] = {};
    uint32_t extent_width = 0, extent_height = 0;
    double viewport[6] = {};
    uint64_t scissor[4] = {};
    float depth_bias[2] = {};
    float blend_color[4] = {};
  };
  EncoderFixedState encoder_fixed_state_;
  // Last descriptor index plan per shader stage (vertex, pixel): a pure
  // function of the shader's binding lists, asked on every draw.
  struct DescriptorPlanMemo {
    const void* shader = nullptr;
    const void* textures = nullptr;
    const void* samplers = nullptr;
    size_t texture_count = 0, sampler_count = 0;
    uint32_t plan_count = 1;
    bool plan_valid = true;
  };
  DescriptorPlanMemo descriptor_plan_memo_[2];
  static bool StateDedupeEnabled();
  static bool EncoderDedupeEnabled();
  static bool HostReadFastEnabled();
  void ResetEncoderDedupe();
  // Draw-pass census (XE_METAL_PASS_CENSUS=1): fed at every pass begin, draw
  // and pass end; reported on the frame-stats tick and at lifecycle sites.
  metal::MetalPassCensus pass_census_;
  // Guest PS exports depth (oDepth) but the draw binds no depth target: Metal
  // rejects such pipelines ("shader writes to depth" with no attachment), and
  // the draw used to be NULL-PIPELINE skipped — Sonic's fullscreen bloom
  // composite (draw 1088) fell to this, leaving stale mip debris in the big
  // RT (the top-band scramble). Attach a throwaway scratch depth target
  // instead (DontCare/DontCare, default depth state = test Always, write off
  // — hardware semantics: z disabled ignores oDepth). Keyed by (w,h);
  // id<MTLTexture>, retained.
  bool dummy_depth_pass_ = false;
  // Set while encoding a memexport-only draw: BeginFrameEncoder builds an
  // attachment-less pass (renderTargetWidth/Height = 1) instead of requiring
  // bound render targets.
  bool memexport_pass_ = false;
  // Set while encoding a pixel-shader memexport draw that binds no render
  // target: BeginFrameEncoder attaches only a scratch depth of
  // pixel_export_extent_ (guest scissor, power-of-two bucketed).
  bool pixel_export_pass_ = false;
  uint32_t pixel_export_extent_[2] = {};
  std::unordered_map<uint64_t, void*> dummy_depth_textures_;

  // RAM-frontbuffer present fallback (the mainline/XeniOS RequestSwapTexture
  // pattern, FMV slice): when a swap has neither an fb snapshot nor a
  // rendered target, the frontbuffer content lives only in guest RAM
  // (CPU-decoded video). Untile+upload it as the present source; content
  // hash skips the re-upload on unchanged frames.
  void* GetRamFrontbufferTexture(uint32_t base, uint32_t width,
                                 uint32_t height);
  void* fb_ram_texture_ = nullptr;  // id<MTLTexture>, shared storage, retained.
  uint64_t fb_ram_content_hash_ = 0;
  uint32_t fb_ram_width_ = 0;
  uint32_t fb_ram_height_ = 0;

  // Bring-up diagnostic: pair the real guest vertex function with a hardcoded
  // solid-color fragment function to isolate vertex-side (positions/constants)
  // from pixel-side (guest PS / textures). debug_solid_fragment_ is the shared
  // id<MTLFunction>; pipelines are cached by guest vertex-function pointer.
  void* debug_solid_fragment_ = nullptr;
  std::unordered_map<void*, void*> debug_solid_pipelines_;

  // Set when at least one draw rendered into the RT this frame, so IssueSwap
  // presents the RT rather than the bring-up test pattern. Reset each swap.
  bool frame_has_render_ = false;

  // Number of swaps issued; drives the bring-up test pattern color so on-device
  // it's visually obvious that frames are flowing through the guest-output path.
  uint32_t swap_count_ = 0;
  // ::rex::graphics::metal::GetMetalSettings().metal_async_pipelines.Get() attribution: draws that could not use a stand-in
  // and waited (target not recurring, small target, memexport).
  uint64_t async_wait_counts_[3] = {};
  double async_wait_ms_[3] = {};
  uint32_t async_wait_logged_swap_ = UINT32_MAX;
  // The previous swap's frontbuffer (guest base, bytes): resolve census and
  // the snapshot policy use it to tell frontbuffer resolves apart.
  uint32_t last_frontbuffer_base_ = 0;
  uint32_t last_frontbuffer_bytes_ = 0;
  // The last few DISTINCT frontbuffer ranges (double/triple buffering), for
  // the snapshot policy: resolves overlapping one keep their snapshot, which
  // presentation reads.
  std::array<std::pair<uint32_t, uint32_t>, 4> recent_frontbuffers_ = {};
  bool ResolveOverlapsRecentFrontbuffer(uint32_t start, uint32_t length) const;
  static bool ResolveSnapshotsForAllEnabled();
  // Draws successfully encoded (across the whole run) — logged periodically so
  // on-device we can tell real geometry submission from the clear-only path.
 public:
  // Swap-path wall time since the last stats line, and its worst single swap.
  uint64_t swap_wall_total_ns_ = 0;
  uint64_t swap_wall_max_ns_ = 0;
  uint32_t swap_wall_count_ = 0;
  // Total time in PM4 execution since the last stats line; the command processor's own share of a frame.
  uint64_t pm4_exec_ns_ = 0;

 private:

  uint32_t draw_count_ = 0;
  uint32_t draw_skip_count_ = 0;

  // Reason-coded skip classification behind draw_skip_count_. Records the
  // current draw's primitive/shader identity at each skip site; reported at
  // swap and consulted by trace triggering. See metal_draw_skip_accounting.h.
  MetalDrawSkipAccounting draw_skip_accounting_;
  // The one place every skip site goes through: bumps draw_skip_count_,
  // records the reason, and emits a one-shot log line for actionable sites.
  void RecordDrawSkip(MetalDrawSkipReason reason, const char* site);

  // Condition-triggered next-frame capture (XE_METAL_AUTO_TRACE, default
  // off): evaluated at swap against this frame's actionable skips and GPU
  // faults; a fire arms RequestFrameTrace so the NEXT frame is captured.
  // Lazily constructed so env parsing happens once, on the GPU thread.
  std::unique_ptr<MetalAutoTraceTrigger> auto_trace_trigger_;
  // Faulted command buffers drained since the previous swap (GPU thread
  // only: bumped in DrainFaultedCommandBuffers, reset in FinalizeSwapFrame).
  uint32_t frame_gpu_fault_count_ = 0;
  // Swap-boundary finalization (trigger evaluation + per-frame counter
  // reset). MUST run on every IssueSwap return path — the suppressed-present
  // and no-presenter/no-queue early exits included — or faults/skips carry
  // into a later successful swap and get misattributed.
  void FinalizeSwapFrame();
  // Monotonic per-finalization index for trigger cooldown math; swap_count_
  // is unsuitable because it does not advance on suppressed frames.
  uint64_t swap_finalize_index_ = 0;

  // Zero-copy runtime cost counters (XE_METAL_ZERO_COPY_REPORT=1, default off
  // and inert until enabled in SetupContext). Advanced at the production
  // operations themselves -- the commit, the wait that blocked, the barrier
  // that drained, the retirement -- so the evidence line reports what happened
  // rather than what was configured. Nothing in the rendering or authority path
  // reads it.
  metal::ZeroCopyCostMeter zero_copy_cost_;

  // Submission-boundary attribution (XE_METAL_SEAM_TIMING=1). Fixed arrays,
  // GPU-thread only, accumulated only when the gate is on; emitted per swap
  // from FinalizeSwapFrame and finally from ShutdownContext (covers zero-swap
  // trace runs), reset after each emission. Pure accounting -- nothing in the
  // rendering or authority path reads it.
  metal::SeamTimingAccumulator seam_timing_;

  // Need-based consumer waits engagement meter (design D3). UNCONDITIONAL --
  // unlike seam timing and the zero-copy cost meter it has no enable switch,
  // because the D5 gate reads engagement from it and a counter that only counts
  // when armed cannot tell "the feature never engaged" from "the meter was
  // off". Advanced only from the GPU thread's consumer corridor, emitted from
  // the same lifecycle points as the seam report, and NOT reset there: it is a
  // session total, and it is also the flag's banner for a gate arm. Nothing in
  // the rendering or authority path reads it.
  metal::NeedBasedWaitMeter need_wait_meter_;
  // Formats + logs the meter under `site`. Emitted whenever the flag is on
  // (an all-zero ON line is the arm-invalidity evidence D5 asks for) or the
  // meter saw any request at all.
  void EmitNeedBasedWaitReport(const char* site);

  // Bounded, deduplicated reporting of memexport producer classes this backend
  // does not route. Reset at the title/session boundary; holds no guest state
  // and takes no part in cache clearing.
  MemexportGapReporter memexport_gap_reporter_;

  // Contract 1 census state. Raw write counts are separated from completed
  // ramp changes: engagement requires a COMPLETE non-identity ramp observed at
  // IssueSwap, not a hook count.
  // --- Standard 256-entry guest gamma ramp application (default OFF) ---
  //
  // Starts DIRTY so the first swap uploads whatever the base class seeded,
  // without relying on a virtual hook having fired during base initialization
  // (it does not -- CommandProcessor::Initialize() writes the default table
  // directly). Re-armed at the title/session boundary for the same reason.
  bool gamma_ramp_dirty_ = true;
  uint64_t gamma_ramp_uploads_ = 0;
  uint64_t gamma_ramp_applications_ = 0;
  // Gamma census state (see CensusGammaState).
  uint64_t gamma_pwl_writes_ = 0;
  uint64_t gamma_census_signature_ = 0;
  uint64_t gamma_census_reports_ = 0;
  // id<MTLBuffer>, 256 dwords. Mutated only between swaps; see the lifetime
  // invariant documented at MetalUploadGammaRamp.
  void* gamma_ramp_buffer_ = nullptr;
  // id<MTLTexture>, the pre-ramp copy of the selected swap source.
  void* gamma_ramp_source_texture_ = nullptr;
  uint32_t gamma_ramp_source_width_ = 0;
  uint32_t gamma_ramp_source_height_ = 0;
  // id<MTLRenderPipelineState>.
  void* gamma_ramp_pipeline_ = nullptr;
  // True between recording a ramp-reading pass and its completion wait. Exists
  // so the "IssueSwap always waits" invariant is asserted rather than assumed.
  bool gamma_ramp_upload_in_flight_ = false;
  // Post-process anti-aliasing of the guest output (SwapPostEffect::kFxaa and
  // kFxaaExtreme), run on the swap mailbox after gamma and before the
  // presenter scales it. command_buffer is an id<MTLCommandBuffer> and
  // mailbox an id<MTLTexture>. Returns false (mailbox untouched) if the
  // pipeline or the copy could not be created.
  bool EncodeSwapFxaa(void* command_buffer, void* mailbox, bool extreme);
  // id<MTLRenderPipelineState>: [0] FXAA, [1] FXAA extreme; built for
  // swap_fxaa_pipeline_format_ (an MTLPixelFormat).
  void* swap_fxaa_pipelines_[2] = {};
  uint64_t swap_fxaa_pipeline_format_ = 0;
  bool swap_fxaa_pipeline_failed_[2] = {};
  // id<MTLSamplerState>, linear, clamp to edge.
  void* swap_fxaa_sampler_ = nullptr;
  // id<MTLTexture>: the copy of the mailbox the pass reads.
  void* swap_fxaa_source_texture_ = nullptr;
  uint64_t swap_fxaa_applications_ = 0;
  // Stage 3 (optimize-metal-gpu-resident-resolve-coherence,
  // XE_METAL_SWAP_LIFETIME, default on, exact-0 rollback): the swap no longer drains the GPU. The frame
  // command buffer is committed without waiting and the CPU waits only for
  // the submission recorded two swaps earlier (bounded run-ahead of two
  // frames). The present's gamma-ramp buffer becomes a ring so its upload
  // never races the GPU and the presenter blit needs no completion wait
  // (the presenter shares the command queue: queue order suffices).
  static bool SwapLifetimeEnabled();
  static constexpr uint32_t kFrameLifetimeRing = 3;
  uint64_t frame_lifetime_submissions_[kFrameLifetimeRing] = {};
  static constexpr uint32_t kGammaRampRing = 9;  // > max in-flight (8)
  void* gamma_ramp_ring_[kGammaRampRing] = {};
  // PWL guest gamma (metal_pwl_gamma.h): a per-swap ring of 128x3-dword
  // tables and the pipelines for one-slice-array and 2D sources.
  void* pwl_ramp_ring_[kGammaRampRing] = {};
  void* pwl_pipeline_array_ = nullptr;  // id<MTLRenderPipelineState>
  void* pwl_pipeline_2d_ = nullptr;     // id<MTLRenderPipelineState>
  uint64_t pwl_gamma_applications_ = 0;
  // True when the guest selected the PWL ramp this swap (metal_pwl_gamma.h).
  bool PwlGammaSelected();
  // This swap's PWL table (id<MTLBuffer>), filled from the guest registers.
  void* PwlRampBufferForSwap(void* device);
  // id<MTLRenderPipelineState> drawing into `pixel_format` from an array or
  // 2D source; nullptr if it cannot be created.
  void* PwlGammaPipeline(void* device, uint64_t pixel_format, bool array);
  // Frame command buffers committed (each = commit + waitUntilCompleted);
  // per-frame average logged by MetalFrameStats at IssueSwap.
  uint32_t frame_commit_count_ = 0;
  // Diagnostic (XE_METAL_UNTILE_TIMING=1): wall time inside
  // DrainPendingWritebacks (RAM publication) since the last frame-stats tick.
  uint64_t publish_ns_ = 0;
  uint32_t publish_calls_ = 0;

  // S7 — submission-gated lifetime. Every frame-command-buffer commit gets a
  // monotonically increasing submission id; a completion handler publishes
  // the highest completed id (also published inline after synchronous waits,
  // which don't order against the async handler). Reclamation work enqueues
  // as (submission, action) and runs from PumpCompletedActions once its
  // submission has completed. While EndFrameEncoder still waits
  // synchronously this is behavior-neutral; it is the prerequisite for S8
  // (removing the hot-path waits) — arenas/slots/textures must not be
  // reclaimed while a submission that references them is in flight.
  // Commits per seam reason since the last frame-stats line; see MetalCommitReasons.
  uint32_t commit_reason_counts_[size_t(metal::SeamBoundaryReason::kCount)] = {};

  uint64_t submission_counter_ = 0;
  // XE_METAL_GPU_BUSY (see the .mm): GPU busy time of frame submissions.
  static bool GpuBusyTimingEnabled();
  static void NoteGpuBusyInterval(double start, double end);
  static double ConsumeGpuBusySeconds();
  // Read lock-free (acquire) from everywhere; ADVANCED only by
  // PublishCompletedSubmission, and only while holding completion_mutex_. That
  // mutex is the truth boundary WaitForSubmission's locked re-check reads
  // across — see PublishCompletedSubmission for why the advance may not sit
  // beside the lock instead of inside it.
  std::atomic<uint64_t> completed_submission_{0};

  // Deferred cache clearing. The request is latched by ClearCaches() and
  // executed at the frame boundary (end of IssueSwap), where the ordering
  // contract's preconditions already hold: the frame's single synchronous
  // EndFrameEncoderAndWait has finished the encoder, awaited every submission,
  // and pumped completion actions, and the full-domain
  // ResolveRamConsumerDependencies has published every pending memexport and
  // render-target writeback to guest RAM. Eligibility is decided by the shared
  // device-free predicate so the rules are testable as values.
  //
  // `publication_failed` (passed in by the caller) keeps the request latched
  // when a swap suppressed its present because publication did not converge:
  // clearing over unpublished guest data would be silent data loss, not a
  // cache eviction.
  void ExecuteDeferredCacheClear(bool publication_failed);
  // Step 5 of that ordering: release derived cache state in the one order that
  // keeps cross-cache retirements valid --
  //   shared texture subclass (retires binding slots into the legacy pools)
  //   -> derived resolve-bridge classification (retires bridge slots/views
  //      into the same pools, and names the snapshot each bridge held)
  //   -> exact-identity snapshot release plus the command-processor-owned
  //      per-base metadata for the bases that actually matched
  //   -> legacy texture-cache DRAIN (frees everything retired above)
  //   -> render targets -> shared memory -> Metal-owned pools.
  // Every step that retires into the legacy pools precedes the drain.
  // Callable ONLY from the kExecute path, which is what proves the device is
  // idle and every GPU-produced guest write has already reached RAM. Returns
  // false without releasing anything if a precondition fails, in which case
  // the request must stay latched and must NOT be counted as executed.
  bool ReleaseCachesForClear();
  bool cache_clear_requested_ = false;
  // Clears that COMPLETED -- ordering satisfied and resources released.
  // Consumed by CommandProcessor::cache_clears_executed() so a scripted gate
  // can prove the request was honored: a cleared cache is observationally
  // neutral, so identical output is also exactly what a discarded request
  // produces.
  //
  // Atomic because the reader is not this thread: the GPU worker will advance
  // it inside IssueSwap, while a gate reads it after
  // TracePlayer::WaitOnPlayback. That wait is a threading-event wait whose C++
  // happens-before relationship with the write is not specified by anything
  // this code controls, so the value is published explicitly (release/acquire)
  // instead of assumed.
  //
  // Advanced by the resource-release step (section 3 of the owning change),
  // NOT by admission -- it certifies a completed clear, and a gate that reads
  // it must not be told a clear happened when nothing was released. It is
  // therefore 0 for as long as release is unimplemented, which correctly fails
  // the engagement gate.
  std::atomic<uint32_t> cache_clears_executed_{0};
  // Replay resets that EXECUTED (quiescence held and retirement ran).
  // Telemetry for the replay/test-only guard: a live run must end with 0.
  std::atomic<uint32_t> replay_resets_executed_{0};
  std::deque<std::pair<uint64_t, std::function<void()>>> completion_actions_;
  // Submissions whose command buffer FAULTED (GPU error status): their GPU
  // work never executed, yet the submission still publishes as completed
  // for reclamation. Recorded (GPU-thread only; bounded) as the seed
  // evidence for the command-buffer fault-attribution task, which
  // attributes the first failing submission by encoder label and resource
  // class before any lifetime fix.
  std::unordered_set<uint64_t> faulted_submissions_;
  void PublishCompletedSubmission(uint64_t submission);
  void PumpCompletedActions();
  void DumpBoundColorRtDiagnostic(const ::rex::gpu::Shader* pixel_shader);
  void ServiceLazyCommitRequest();
  void OnIdlePoll() { ServiceLazyCommitRequest(); }
  void OnWaitRegMemStalled();
  uint64_t WaitRegMemStallUs() const;

  // S8 — async submission. EndFrameEncoder commits WITHOUT waiting (unless
  // XE_METAL_SYNC_WAIT=1 restores the old synchronous behavior for A/B);
  // sites that read results on the CPU right after the commit use
  // EndFrameEncoderAndWait. Faults are recorded by the completion handler
  // and drained (logged + auto-recovered) on the GPU thread from
  // PumpCompletedActions.
  void EndFrameEncoderAndWait(metal::SeamBoundaryReason reason);
  // Blocks until `submission` completes (condition variable signaled by the
  // completion handler). Returns false on timeout — a wedged GPU.
  // `out_did_block`, when non-null, is set true ONLY on the locked re-check
  // path that proves the condition-variable wait was actually entered — the
  // single exact blocking fact this seam can state (the unlocked fast-path
  // pre-check races with completion and must never be used as "blocked").
  bool WaitForSubmission(uint64_t submission, uint32_t timeout_ms = 4000,
                         bool* out_did_block = nullptr);
  // XE_METAL_SEAM_TIMING=1 (exact, warn-once fail-closed): submission-boundary
  // attribution. A timing-enabled run is an ATTRIBUTION condition, not
  // comparable to the pinned performance baselines.
  static bool SeamTimingEnabled();
  // WaitForSubmission wrapped with direct-wait attribution (seam timing's
  // third layer, frametime 3.1): times the call and records factual
  // blocked-ness under the given site tag. Behavior-identical to a bare
  // WaitForSubmission when the gate is off.
  bool SeamTimedDirectWait(metal::SeamDirectWait site, uint64_t submission);
  // Formats + logs the seam accumulator under `site`, then resets it. No-op
  // when the gate is off or nothing accumulated.
  void EmitSeamTimingReport(const char* site);
  // Arena allocation with ring-full handling: on failure, waits for the
  // oldest in-flight submission, reclaims, and retries once.
  bool ArenaAllocate(uint32_t size, uint32_t alignment,
                     MetalUploadAllocation* out);
  void DrainFaultedCommandBuffers();
  // Guards the completed_submission_ ADVANCE as well as the waiters' re-check,
  // so "did this submission complete" has one answer inside it. Held only
  // across atomic loads/stores and the meter's lock-free count — never across
  // GPU work, a callback, or a log — so the Metal completion handler that
  // takes it cannot be delayed by a waiter.
  std::mutex completion_mutex_;
  std::condition_variable completion_cv_;
  std::mutex fault_mutex_;
  // ONE record per faulted completion. Previously two parallel vectors whose
  // index correspondence held only by convention (both pushed under
  // fault_mutex_ in the same handler); a single record makes the pairing
  // structurally unbreakable rather than an invariant a future edit can
  // silently violate.
  struct FaultedCompletion {
    void* command_buffer = nullptr;  // id<MTLCommandBuffer>, retained.
    uint64_t submission = 0;         // Serial of this failed completion.
    // Queue epoch captured AT SUBMISSION, not at drain. A callback can arrive
    // after the queue was revoked and recreated; keying its episode on the
    // drain-time epoch would split one episode across two.
    uint64_t submit_epoch = 0;
  };
  std::vector<FaultedCompletion> faulted_completions_;

  // FAULT EPISODES. A single GPU failure makes every in-flight submission
  // complete with an error, so failed-serial counts over-report root events.
  // An episode is therefore keyed by the QUEUE EPOCH (bumped on every
  // revoke/recreate), not by arrival order: all failures observed within one
  // epoch belong to one episode.
  //
  // WHY NOT "first callback wins": completion callbacks may arrive OUT OF
  // SUBMISSION ORDER, so the first-reported failure is not necessarily the
  // earliest one. The episode ROOT is the LOWEST failed submission serial seen
  // within the episode, revised downward if a lower serial reports later.
  // Pure state machine (metal_fault_episode.h) so the grouping rules are
  // pinned by the L0 suite rather than only exercised when a stochastic
  // hardware fault happens to occur.
  FaultEpisodeTracker fault_episodes_;
  // OUTSTANDING CALLBACKS PER EPOCH. An episode's root/count stay provisional
  // until every submission issued under its epoch has reported, so finalizing
  // needs this count. Incremented on the submitting thread, decremented in the
  // completion handler (arbitrary thread) -- hence atomic, and indexed by
  // epoch slot so the success path takes no lock and logs nothing.
  static constexpr uint32_t kFaultEpochSlots =
      FaultEpisodeTracker::kMaxTrackedEpochs;
  std::atomic<uint32_t> epoch_outstanding_[kFaultEpochSlots] = {};
  // Set once capacity is exhausted: capture is disabled rather than evicting
  // attribution that may still receive a late callback.
  bool fault_capture_disabled_ = false;
  uint64_t NoteFaultEpisodeMember(uint64_t submission, uint64_t submit_epoch);
  void AssertFaultEpochOwnership() const;
  void MarkFaultEpochClosed(uint64_t epoch);
  void SweepFinalizedFaultEpisodes();

  // ZPD occlusion queries. The shared CommandProcessor owns the whole logical
  // report state machine (slots, segment splitting, normalization, guest
  // writes); everything here is the Metal resource and encoder half of that
  // contract. Ordering follows Canary D3D12/Vulkan: resume the active segment
  // when an encoder opens, close it when the encoder ends, resolve only after
  // the owning submission completes.
  //
  // Metal requires visibilityResultBuffer on the render pass DESCRIPTOR, so a
  // segment can only run on an encoder that was built with the buffer already
  // attached -- hence encoder_has_zpd_visibility_ rather than a global "pool
  // exists" test.
  void EnsureZPDQueryResources();
  void ShutdownZPDQueryResources();
  bool IsZPDQueryPoolReady() const;
  bool CanOpenZPDQuery() const;
  QueryOpenResult OpenZPDQuery(ReportHandle report_handle,
                               bool can_close_submission);
  bool CloseZPDQuery(ReportHandle report_handle,
                     uint64_t& out_submission);
  bool DiscardZPDQuery();
  void PumpQueryResolves();
  bool AwaitQueryResolve(ReportHandle report_handle,
                         uint64_t wait_for_submission);
  uint64_t GetCompletedSubmission() const {
    return completed_submission_.load(std::memory_order_acquire);
  }
  void PollCompletedSubmission() { PumpQueryResolves(); }

  // Ends the active query on the current encoder and records where its result
  // will land. `report_handle` is kInvalidReportHandle for a discard, whose
  // slot must still ride the resolve queue: Metal allows a given visibility
  // offset to be selected only once per pass, so the slot cannot return to the
  // free list until its submission has completed.
  bool RetireActiveZPDQuery(ReportHandle report_handle,
                            uint64_t* out_submission);

  // True when a logical report is open and waiting for a host segment --
  // either its first, or a resume after CloseQuerySegment marked it pending
  // at the previous encoder boundary.
  bool ZPDSegmentPending() const;

  struct MetalZPDResolve {
    uint64_t submission = 0;
    uint32_t index = 0;
    uint32_t generation = 0;
    ReportHandle report_handle{};
  };
  metal::MetalZPDVisibilityPool zpd_visibility_pool_;
  std::deque<MetalZPDResolve> zpd_resolves_in_flight_;
  // Handles of GUEST logical reports observed still open at a successful
  // cache-clear execution -- the one ZPD state a clear could strand, since the
  // pool and its in-flight resolves are retained but a live report must still
  // retire afterwards. Never holds a merely in-flight resolve. Populated only
  // when that state actually occurs (no natural fixture does), and drained by
  // PumpQueryResolves when the shared retirement path completes, so ordinary
  // resolves cost one empty-set branch. Cleared at teardown.
  std::unordered_set<ReportHandle> zpd_handles_spanning_clear_;
  uint32_t zpd_active_index_ = 0;
  uint32_t zpd_active_generation_ = 0;
  bool zpd_query_open_ = false;
  // Whether the currently open render encoder was built with the visibility
  // buffer attached. False for every encoder created before a guest first
  // opened a report, which is what makes a no-query title pay nothing.
  bool encoder_has_zpd_visibility_ = false;

  // Geometry-emulation (rect-list) diagnostics: draws dispatched via the mesh
  // path, and times the geometry pipeline failed to build (fell through to skip).
  uint32_t geo_draw_count_ = 0;
  // Command buffers that completed with an error this session (every one,
  // not only the logged ones; reported on the MetalFrameStats line).
  uint32_t cb_error_count_ = 0;
  // Seam boundary reason of the last 64 submissions, by submission serial
  // (fault attribution: why the faulted command buffer was committed).
  uint8_t submission_reason_ring_[64] = {};
  // Shader pairs of the last 8192 draws, by submission serial (consecutive
  // repeats counted): names what a faulted command buffer was running.
  struct DrawPairNote {
    uint64_t submission;
    uint64_t vs, ps;
    uint32_t count;
    uint32_t prim;
    // draw_count_ when the first of the run was issued (encoder labels
    // carry the same counter as draws@).
    uint32_t first_draw;
  };
  DrawPairNote draw_pair_ring_[8192] = {};
  // Rectangle-VS-route draws, for the fault report: which pipeline each used.
  struct RectDrawNote {
    uint64_t submission;
    uint64_t ps;
    void* pipeline;
    uint32_t draw;
    uint32_t index_count;
    uint32_t color_format;
    uint32_t depth_format;
  };
  RectDrawNote rect_draw_ring_[256] = {};
  uint32_t rect_draw_next_ = 0;
  // XE_METAL_SLOT_AUDIT: the current draw's texture slots.
  std::vector<uint32_t> audit_draw_slots_;
  uint32_t draw_pair_next_ = 0;
  // Draws by guest primitive type and the path that issued them (census):
  // 0 native, 1 auto quad->tri, 2 DMA quad->tri, 3 auto line loop,
  // 4 geometry emulation, 5 tessellation, 6 unmappable (skipped).
  uint32_t prim_census_[32][8] = {};
  uint32_t geo_pipeline_fail_count_ = 0;
  // How many indexed triangle-list draws we've dumped diagnostics for.
  uint32_t tri_log_count_ = 0;
  // Most recent color-resolve (IssueCopy) destination range in guest memory, for
  // the RT-as-texture bridge: a texture fetch whose base lands here is asking for
  // the scene we rendered into the color RT.
  uint32_t last_color_resolve_base_ = 0;
  uint32_t last_color_resolve_start_ = 0;
  uint32_t last_color_resolve_length_ = 0;

  // Stage-3 slice: SMALL resolves also write back into REAL guest memory at
  // frame end (staging blit + CPU memcpy). JC2's auto-exposure loop runs
  // through the CPU: the luminance pyramid resolves to RAM, game code READS
  // the 1x1 result and computes the next frame's exposure constant — the
  // snapshot bridge alone leaves that RAM unwritten (black/stale exposure).
  struct PendingWriteback {
    void* staging;  // id<MTLBuffer>, retained.
    uint32_t guest_addr;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;     // destination bytes per texel (1, 2, 4, 8, or 16).
    // Scatter operation endian. Zero when a canonical packed kernel already
    // produced guest-endian bytes; otherwise the Endian128 value to apply.
    uint32_t endian;
    // Guest layout provenance retained even when staging is already endian-
    // transformed (used by presentation, logs, and sampling metadata).
    uint32_t layout_endian;
    // Tiled-dest layout (stage 3): resolve destinations are TILED guest
    // textures — the drain scatters rows through the tiled addresser (a
    // linear memcpy made every tiled fetch of the dest read block garbage).
    uint32_t dest_pitch_aligned;  // texels
    uint32_t dest_offset_x;
    uint32_t dest_offset_y;
    uint32_t dest_extent_bytes;   // write bound relative to guest_addr
    uint32_t dest_height_aligned;
    uint32_t dest_slice;
    uint32_t dest_is_3d;
    // RB_COPY_DEST_INFO.copy_dest_swap: hardware packs resolved pixels .bgra
    // (artist-texture channel order) — the scatter must too, or every fetch
    // of the dest through the game's swizzle reads R/B swapped.
    uint32_t swap_rb;
    // Guest layout provenance, independent of whether the snapshot shader has
    // already applied the swap before staging (and CPU scatter therefore must
    // not apply it again).
    uint32_t layout_swap_rb;
    // Staging already contains final guest-format, guest-endian texel bytes.
    uint32_t packed_bytes;
    // Submission whose command buffer fills `staging` — the drain may only
    // read the staging contents once this submission has completed.
    uint64_t submission;
    // Resolve provenance for the backend-neutral ResolveWB record
    // (XE_GPU_RESOLVE_WB_LOG): RB_COPY_DEST_INFO.copy_dest_format, the draw
    // index of the issuing resolve, and the swap it was enqueued on (drain
    // swap - enqueue swap = data age in frames).
    uint32_t dest_format;
    uint32_t draw_index;
    uint32_t enqueue_swap;
    // DEPTH resolve writeback: staging holds R32Float depth from the snapshot
    // conversion; the drain packs each float to the guest D24S8 word
    // (depth24 << 8, stencil 0 in v1) before the endian scatter.
    // 0 = color (no conversion), 1 = unorm24 (k_24_8), 2 = float24 (20e4).
    uint32_t depth_encode;
    // Monotonic cross-kind registration order for authority-seam chronology.
    // This lives on the existing pending record; it is not a second queue.
    uint64_t authority_sequence;

    // Stage B (implement-metal-gpu-visible-resolve-authority): the GPU scatter
    // already wrote this record's bytes into the SHARED BUFFER at their guest
    // tiled addresses, so `staging` is null and the record is GPU-current. It
    // still owes guest RAM its bytes; publication copies them buffer->RAM by
    // repeating the same tiled walk, and only that publication retires it.
    //
    // DEFAULTED and declared LAST on purpose: every existing construction site
    // uses a positional aggregate initializer, so a field inserted mid-struct
    // silently shifts `submission` into a uint32 slot (it does not even
    // compile cleanly -- it narrows). Keep new fields here.
    uint32_t gpu_current = 0;
    // Stage 2 (optimize-metal-gpu-resident-resolve-coherence): the record's
    // pages are read-watched; RAM publication happens on the first CPU touch
    // (MetalLazyPublication) or on an eager drain, whichever comes first.
    uint32_t lazy_armed = 0;

    // Guest-memory extent this writeback touches, for overlap / wait / drain-
    // filter tests. Tiled pitch, offsets, and row padding make the real extent
    // larger than a packed width*height*bpp span, so prefer the addresser-
    // computed dest_extent_bytes when it was carried; fall back to the packed
    // span only when it is zero (pre-tiled-layout enqueue paths).
    uint32_t range_bytes() const {
      return dest_extent_bytes ? dest_extent_bytes : width * height * bpp;
    }
  };
  std::vector<PendingWriteback> pending_writebacks_;
  // Per-draw consumer requests, kept across draws so their range vectors
  // keep their capacity (IssueDraw).
  metal::ConsumerRequest vertex_request_scratch_;
  metal::ConsumerRequest texture_request_scratch_;
  // Texture-dependency memo (XE_METAL_TEX_DEP_MEMO): draws whose shader pair
  // and bound fetch words were already found to have NO dependency on any
  // pending producer, while the producer set, the completed submission, the
  // resolved-target registry and trace capture are all unchanged. Any change
  // to that state clears it. Burnout: ~90% of texture consultations are
  // empty and 88% of bindings repeat.
  struct TexDependencyMemo {
    uint64_t sequence = ~uint64_t(0);
    uint64_t completed = ~uint64_t(0);
    uint64_t registry_epoch = ~uint64_t(0);
    size_t writebacks = ~size_t(0);
    size_t memexports = ~size_t(0);
    bool trace_open = false;
    // hash -> exact key (shader pointers, bound mask, bound fetch words):
    // index maps the hash to a span of key_pool. Flat so clearing keeps the
    // storage (the node-per-insert map was ~1% of the Mac Pro CP thread).
    struct KeySpan {
      uint32_t offset;
      uint32_t length;
    };
    MetalFlatIndexMap index;
    std::vector<KeySpan> spans;
    std::vector<uint32_t> key_pool;
    std::vector<uint32_t> key_scratch;
    bool Empty() const { return spans.empty(); }
    void Clear() {
      index.Clear();
      spans.clear();
      key_pool.clear();
    }
    bool Contains(uint64_t hash, const std::vector<uint32_t>& key) {
      const uint32_t* i = index.Find(hash);
      if (!i) {
        return false;
      }
      const KeySpan& span = spans[*i];
      return span.length == key.size() &&
             std::equal(key.begin(), key.end(),
                        key_pool.begin() + span.offset);
    }
    void Insert(uint64_t hash, const std::vector<uint32_t>& key) {
      // Overwritten keys stay in the pool until the next clear; bound it.
      if (key_pool.size() + key.size() > (size_t(1) << 20)) {
        Clear();
      }
      spans.push_back({uint32_t(key_pool.size()), uint32_t(key.size())});
      key_pool.insert(key_pool.end(), key.begin(), key.end());
      index.Insert(hash, uint32_t(spans.size() - 1), true);
    }
    uint64_t hits = 0, misses = 0, inserts = 0, clears = 0;
    uint64_t verify_checked = 0, verify_mismatches = 0;
  };
  TexDependencyMemo tex_dependency_memo_;
  metal::GuestRangeSet unserved_ranges_scratch_;

  // Memexport draws executed this frame whose exports are not yet coherent in
  // guest RAM. The shared-memory buffer is MTLStorageModeShared, so the drain
  // is a plain memcpy from its contents once `submission` has completed
  // (exports are linear and already guest-endian — no scatter, no swap).
  struct PendingMemexport {
    uint32_t base_address;  // guest physical
    uint32_t size_bytes;
    uint64_t submission;
    uint64_t authority_sequence;
    // Stage 2: read-watched; RAM gets the bytes on first CPU touch.
    uint32_t lazy_armed = 0;
    // The range's shared-buffer bytes before the export ran, captured when it
    // is armed at encode time; publication then copies only changed words.
    std::shared_ptr<const std::vector<uint8_t>> pre_image;
  };
  std::vector<PendingMemexport> pending_memexports_;
  // Bytes of staged hazard uploads (StageHazardUpload) whose blits may not
  // have run yet: the shared buffer gets them on the GPU timeline, so a
  // memexport pre-image taken on the CPU overlays them to see the buffer as
  // the export will. Pruned once their submission completes.
  struct StagedUploadImage {
    uint32_t offset;
    uint64_t submission;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
  };
  std::deque<StagedUploadImage> staged_upload_images_;
  // Newest submission whose staged bytes were dropped for the cap; no
  // pre-image is taken while it may still be pending.
  uint64_t staged_upload_images_dropped_through_ = 0;
  // Trace playback only: guest ranges this frame's GPU resolves and memexports
  // wrote (kept over later trace restores; cleared at the frame's swap).
  std::vector<std::pair<uint32_t, uint32_t>> playback_gpu_written_;
  // MSAA fast-clear fold census (XE_METAL_MSAA_CLEAR_FOLD_LOG), indexed by
  // metal::MsaaClearFoldRefusal (kNone = folded); reset each report.
  uint64_t msaa_fold_census_[32] = {};
  // Of the folded: depth clears drawn into the colour view.
  uint64_t msaa_fold_colour_alias_census_ = 0;
  // CPU vertex-shader evaluation for the fold's value/alignment proof.
  std::unique_ptr<::rex::gpu::ShaderInterpreter> msaa_fold_interpreter_;
  uint64_t memory_authority_sequence_ = 0;
  // Completion-exposure memo (XE_METAL_COMPLETION_EXPOSURE_MEMO=0 rollback):
  // the full-domain publication decision a fence packet triggers depends
  // only on the pending-producer set (size of each queue + the monotonic
  // registration sequence identify it exactly), the GPU's completed
  // submission and the encoding submission. A fence packet that arrives
  // with none of them changed would repeat the previous decision and its
  // (already applied) effects, so it is skipped. Red Dead Redemption's
  // cinematic issues ~2,400 fence packets per frame against ~210 pending
  // GPU-current records: 27k consumer queries and ~85 ms of a 240 ms frame
  // in DecideConsumer (2026-10-04).
  struct CompletionExposureFingerprint {
    uint64_t writebacks = 0, memexports = 0, sequence = 0, completed = 0,
             encoding = 0;
    bool operator==(const CompletionExposureFingerprint& o) const {
      return writebacks == o.writebacks && memexports == o.memexports &&
             sequence == o.sequence && completed == o.completed &&
             encoding == o.encoding;
    }
  };
  CompletionExposureFingerprint completion_exposure_last_;
  bool completion_exposure_valid_ = false;
  uint64_t completion_exposure_runs_ = 0;
  uint64_t completion_exposure_skips_ = 0;
  // Pending-producer index (XE_METAL_PRODUCER_INDEX=0 rollback): the sorted
  // snapshot the consumer decision reads, rebuilt only when the pending set
  // changes (records are immutable after registration, so queue sizes plus
  // the monotonic registration sequence identify the set exactly), plus a
  // 64 KiB-page occupancy bitmap of every pending range. A per-draw query
  // whose ranges touch no occupied page is decided against an empty producer
  // list (the same default decision DecideConsumer's fast path returns)
  // without snapshotting, sorting or scanning the records. Red Dead's
  // gameplay (2026-10-05): ~17,000 queries per frame against ~350 pending
  // records at 1.76 us each = 25-30 ms of a 100 ms command-processor-bound
  // frame (the cinematic, wait-bound, showed no change from it).
  struct ProducerIndex {
    uint64_t writebacks = 0, memexports = 0, sequence = 0;
    bool valid = false;
    std::vector<metal::PendingProducer> producers;
    std::array<uint64_t, 128> pages{};  // 512 MiB / 64 KiB = 8192 bits
    // Per-kind occupancy for IndexRangeOverlapsProducers: the per-binding
    // memexport / GPU-current overlap checks walked every producer on a
    // texture's pages (mostly resolves of the other kind) to answer false
    // (Red Dead: ~2.5% of the command-processor thread with no memexport
    // pending). XE_METAL_KIND_PAGE_BITMAPS=0 uses the shared bitmap.
    std::array<uint64_t, 128> pages_gpu_current{};
    std::array<uint64_t, 128> pages_memexport{};
    // Per-page producer lists (CSR over the 8192 pages): every producer
    // whose range shares a byte with a query range shares a page with it,
    // so the candidate set for a seed scan or a closure step is the union of
    // the touched pages' lists, never the whole queue.
    // Each occupied page's list is page_entries[page_begin[page],
    // page_end[page]); both are written only for pages set in `pages`, so
    // every reader checks the bit first and a rebuild costs the producers'
    // pages, not all 8192.
    std::vector<uint32_t> page_begin;     // 8192 entries
    std::vector<uint32_t> page_end;       // 8192 entries
    std::vector<uint16_t> page_entries;   // producer indices
    std::vector<uint32_t> candidate_stamp, selected_stamp;  // per producer
    uint32_t query_stamp = 0;
    // Reused per query (capacity kept): the selection frontier.
    std::vector<uint32_t> frontier_scratch;
    // Reused across rebuilds (capacity kept).
    std::vector<metal::PendingProducer> snapshot_scratch;
    // Reused per query: page intervals of the request's two range sets.
    std::vector<std::pair<uint32_t, uint32_t>> seed_spans;
    uint64_t rebuilds = 0, queries = 0, fast_empty = 0, indexed_decisions = 0,
             verify_mismatches = 0;
    // The last indexed decision and its inputs (XE_METAL_DECISION_MEMO): a
    // draw run re-asks the same request against an unchanged index snapshot.
    struct DecisionMemo {
      bool valid = false;
      uint64_t rebuilds = 0;
      uint64_t completed = 0;
      metal::ConsumerKind consumer = metal::ConsumerKind::kTexture;
      metal::ConsumerAccess access = metal::ConsumerAccess::kGuestRam;
      bool order_memexport_producers = false;
      std::vector<metal::GuestRange> resolve_ranges, memexport_ranges;
      metal::ConsumerDecision decision;
    } memo;
    uint64_t memo_hits = 0;
    // Work per indexed decision (logged with the counters above): page
    // entries visited by the seed walk and by the predecessor closure, and
    // the selected records.
    uint64_t seed_steps = 0, closure_steps = 0, selected_total = 0;
    // Per-producer predecessor closure as bitsets over producer indices
    // (XE_METAL_CLOSURE_BITS): closure(i) = {i} and the closures of every
    // older producer overlapping i. It depends only on the snapshot, so it
    // is built once per rebuild (lazily, in index order) instead of walked
    // again by every decision. closure_words bits per producer; the first
    // closure_ready producers are built.
    std::vector<uint64_t> closure_bits, selected_bits;
    uint32_t closure_words = 0, closure_ready = 0;
    // Per occupied page, the bitset of producers on it (mask_words per page,
    // at page_mask_slot[page]); 0 mask_words = not built (over 1024 records
    // or XE_METAL_CLOSURE_BITS=0). A seed or closure step ORs its pages'
    // masks and tests each candidate once, instead of visiting a record on
    // every page it spans (Far Cry 2: 17 pages per record).
    std::vector<uint64_t> page_masks, candidate_bits;
    std::vector<uint32_t> page_mask_slot;
    uint32_t mask_words = 0;
  };
  mutable ProducerIndex producer_index_;
  const std::vector<metal::PendingProducer>& IndexedPendingProducers(
      bool* any_page_hit, const metal::ConsumerRequest& request) const;
  // DecideConsumer over the page index: identical result, O(candidates).
  static bool LeanDecisionEnabled();
  static bool ClosureBitsEnabled();
  metal::ConsumerDecision DecideConsumerIndexedImpl(
      metal::ConsumerKind consumer, const metal::ConsumerRequest& request,
      uint64_t completed_submission) const;
  metal::ConsumerDecision DecideConsumerIndexed(
      metal::ConsumerKind consumer, const metal::ConsumerRequest& request,
      uint64_t completed_submission) const;
  // Exact "does [start, end) overlap a pending producer of this kind"
  // through the index (page bitmap pre-check, then the page's candidates).
  // kGpuCurrentShared writebacks or kPendingMemexport records; the per-
  // binding texture classification asked this of the whole queue for the
  // base and mip range of every binding of every draw (Red Dead gameplay:
  // 21 ms of a 66 ms frame, 2026-10-05).
  bool IndexRangeOverlapsProducers(uint32_t start, uint32_t end,
                                   bool gpu_current_writebacks,
                                   bool memexports) const;
  void EnsureProducerIndex() const;

  // Stage 2 demand-driven publication (XE_METAL_LAZY_PUBLISH, default on, exact-0 rollback,
  // default OFF until the counted gate). See metal_lazy_publication.h.
  // Applies metal_eager_publish_title at a title boundary.
  void* lazy_read_callback_handle_ = nullptr;
  std::atomic<bool> lazy_rearm_needed_{false};
  // A guest thread that WRITES over bytes a pending GPU producer still owns
  // while that producer sits in the OPEN (uncommitted) command buffer asks
  // the GPU thread to commit it (the submission id it needs committed); the
  // GPU thread services the request at its next draw/copy boundary.
  std::atomic<uint64_t> lazy_commit_request_{0};
  // Stale-read attribution: the last guest-visible signal since the open
  // command buffer began (0 = none, else GuestSignalKind + 1; 5 = completion
  // exposure), and stale fault reads counted by that signal.
  std::atomic<uint32_t> signal_since_commit_{0};
  std::atomic<uint64_t> stale_by_signal_[6] = {};
  // Set only around the swap / fence-packet full-domain publications: those
  // are the requests whose GPU-current records may be armed instead of
  // copied. Ranged host reads, waits, cache clears and trace capture stay
  // eager.
  bool lazy_full_domain_request_ = false;
  void RegisterGpuResidentResolve(uint32_t base, uint32_t extent_bytes);
  // An older armed export a new one covers and replaces, with the pages it
  // still owned (not yet published) when it was retired.
  struct SupersededMemexport {
    PendingMemexport record;
    std::vector<std::pair<uint32_t, uint32_t>> owned;
  };
  // `capture_pre_image` only at encode time, before the GPU can have run the
  // export; `superseded` seed the new pre-image on the pages they still owned,
  // so their unpublished words still publish.
  void ArmLazyMemexport(PendingMemexport& me, bool capture_pre_image = false,
                        const std::vector<SupersededMemexport>* superseded =
                            nullptr);
  void RearmLazyPublications();
  void OnLazyRead(uint32_t physical_address_start, uint32_t length,
                  MetalPhysicalAccess access);
  // Same-frame consumer visibility (v1) is handled by the stage-3 lazy flush
  // in IssueDraw: a draw whose texture fetches overlap a pending memexport
  // commits + waits + drains before its uploads read guest RAM.

  // XE_METAL_TIP_SNAP_VIEW: per-base derived 1x1 R32F copies of snapshot
  // texel (0,0) (retained id<MTLTexture>; experiment-scoped, freed with the
  // process).
  std::unordered_map<uint32_t, void*> tip_view_textures_;

  // Byte packing used by GPU resolve writebacks at each guest base. RAM
  // frontbuffer presentation reverses this packing back to logical RGBA.
  // Bases with no entry are CPU-produced scanout buffers and retain the
  // legacy ARGB scanout conversion.
  struct RamColorLayout {
    uint32_t endian;
    uint32_t swap_rb;
    // Snapshot generation this layout was recorded for (see
    // resolve_snapshot_generation_). ram_color_layouts_ entries are never
    // erased, so a base once resolved as canonical color keeps a layout entry
    // forever; presenting canonical RAM must only trust it when it still
    // matches the base's current snapshot generation. A later depth or
    // non-encodable resolve at the same base advances the generation without
    // rewriting the layout, so the stale layout is correctly rejected.
    uint32_t generation = 0;
  };
  std::unordered_map<uint32_t, RamColorLayout> ram_color_layouts_;
  // Monotonic per-base resolve generation: bumped after every successful
  // color or depth resolve, including resolves into an existing/coalesced
  // snapshot. A color writeback stamps its RamColorLayout with the value
  // bumped by the same resolve, so layout.generation == current generation
  // iff the newest resolved content at the base produced that layout.
  std::unordered_map<uint32_t, uint32_t> resolve_snapshot_generation_;

  // Gamma-parity diagnostic state (XE_METAL_GAMMA_PARITY=1).
  std::unordered_map<uint32_t, bool> gamma_parity_resolves_;
  std::unordered_set<uint32_t> gamma_parity_reported_;

  // Draws encoded into the current frame command buffer since its last
  // commit — gates the commit-before-resolve submission boundary.
  uint32_t draws_since_commit_ = 0;

  // Scratch for the per-encoder bindless residency sweep (BeginFrameEncoder).
  std::vector<void*> resident_sweep_scratch_;
  // Vertex-fetch renames of the draw being issued (see
  // MetalSharedMemory::RequestVertexFetchRange).
  struct DrawVfetchRename {
    uint32_t fetch_constant;
    uint32_t guest_start;
    uint32_t length;
    uint32_t buffer_offset;
  };
  std::vector<DrawVfetchRename> draw_vfetch_renames_;
  // XE_METAL_DRAW_PHASE_TIMING: per-milestone nanoseconds inside IssueDraw.
  std::map<std::string, uint64_t> draw_phase_ns_;
  uint64_t NoteDrawPhase(const char* label, uint64_t last_ns);
  std::string FormatDrawPhases(uint32_t frames);
  // Residency set (macOS 15+, XE_METAL_RESIDENCY_SET=0 rollback to the
  // per-encoder sweep): every slot-reachable texture is added once (hooked at
  // the bindless slot descriptor write) and removed when its retired object
  // is released; the set is attached to the command queue and committed
  // when dirty at encoder begin. Replaces useResources over the whole
  // texture population (~3,500) on every render encoder (~170 per frame in
  // Red Dead's cinematic: the IOGPU resource-list rebuild was 14% of the
  // command processor's time, 2026-10-04).
  void* residency_set_ = nullptr;  // id<MTLResidencySet> (retained)
  bool residency_set_dirty_ = false;
  std::unordered_set<void*> residency_members_;
  uint64_t residency_adds_ = 0, residency_removes_ = 0,
           residency_commits_ = 0;
  void InitializeResidencySet(void* device_ptr, void* queue_ptr);
  void ShutdownResidencySet(void* queue_ptr);
  // Once per swap: drop members no longer reachable (cache, parked, resolved
  // targets, shared-route views, retired-in-flight). Texture views released
  // outside the legacy drain (the shared route's binding views) would
  // otherwise stay retained by the set together with their parent
  // textures: Red Dead's cinematic grew to 13.8 GB in 3,000 swaps.
  void ReconcileResidencySet();
  uint64_t residency_reconcile_removes_ = 0;

  // Exposure-loop telemetry (XE_METAL_EXPO_TRACE=<tonemap ps ucode hash hex>):
  // per-swap snapshot of the auto-exposure FEEDBACK LOOP's three stages —
  // (1) the smallest luminance writeback value delivered to guest RAM this
  // frame, (2) the tonemap pixel shader's first float constants (the exposure
  // the game computed FROM (1) and fed back in), (3) the presented luma.
  // Correlating the three time series localizes an oscillation to producer,
  // game controller, or present. Written by DrainPendingWritebacks/IssueDraw,
  // emitted as one "MetalExpo" line per swap.
  float expo_last_wb_value_ = 0.0f;
  uint32_t expo_last_wb_words_ = 0;
  float expo_tonemap_c0_[4] = {};
  bool expo_tonemap_seen_ = false;

  // Per-frame autorelease pool for the GPU Commands thread (cycled at every
  // IssueSwap). The thread has no run-loop pool, so autoreleased Metal objects
  // created while encoding (command buffers, encoders, pass descriptors,
  // label strings) otherwise accumulate for the WHOLE session — a multi-hour
  // play session leaked gigabytes and stalled the machine (same class as the
  // capture path's per-owner pool fix). Opaque objc pool token.
  void* frame_autorelease_pool_ = nullptr;

  // Auto-exposure death-spiral guard: JC2's exposure feedback loop reads the
  // small luminance-pyramid writebacks from RAM. Our resolve occasionally
  // delivers an ALL-ZERO tile (failed owner lookup / stale snapshot / clear
  // timing); the game reads 0 -> exposure collapses to 0 -> the scene renders
  // black -> real luminance is 0 -> latched black forever. Real hardware never
  // hands the loop a pure-zero luminance tile, so when a small writeback would
  // write exactly-zero content we instead re-write the last non-zero content
  // seen at that guest address (hold-last-value), which lets exposure ride the
  // gap and recover. Keyed by guest_addr; stores the final guest-endian words.
  std::unordered_map<uint32_t, std::vector<uint32_t>> held_writeback_content_;

  // Upstream's IPhysicalMemory hands out spans rather than host pointers, and this backend wants a pointer at the point of
  // use in about thirty places. The CPU-side view of guest RAM is the Metal shared memory buffer, so translation is one
  // addition here instead of a cast at every call site. Delete it when upstream offers a host pointer again.
  template <typename T = const void*>
  T TranslateGuest(uint32_t address) const {
    return reinterpret_cast<T>(
        const_cast<uint8_t*>(static_cast<const uint8_t*>(shared_memory_->GetCpuBuffer())) + address);
  }

  // --- Zero-page-draw (occlusion) query segment bookkeeping ---
  // Copied from the fork's command processor, whose PM4 layer owned this state before upstream restructured it. The
  // Metal query machinery (admission decision, slot pool, visibility pool) already lives in the metal_zpd_* headers;
  // these are the records it moves between.
  // The fork's logical query report, field for field: it accumulates host samples across every segment a guest report is
  // split into, and carries the last delta so a forced close does not make the slot look fully occluded.
  struct ZPDReport {
    uint64_t accumulated_samples = 0;
    uint64_t first_segment_end_submission = 0;
    uint64_t last_segment_end_submission = 0;
    uint64_t slot_sequence_id = 0;
    uint32_t slot_base = 0;
    uint32_t begin_record = 0;
    uint32_t end_record = 0;
    uint32_t begin_value = 0;
    uint32_t pending_segments = 0;
    uint32_t cached_delta = 0;
    bool has_cached_delta = false;
    bool ended = false;
    bool provisional_written = false;
  };

  struct ActiveZPDSegment {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t slot_base = 0;
    uint32_t begin_record = 0;
    uint32_t end_record = 0;
    bool segment_active = false;
    bool segment_pending_begin = false;
    bool logical_active = false;
  };

  struct PendingZPDSlot {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t cached_delta = 0;
    bool has_cached_delta = false;
  };

  // Query census: which draws the occlusion path measured, skipped or could not measure. The fork kept it on its PM4 layer.
  enum class ZPDCensusDraw { kMeasured, kUnmeasured, kSkipped, kCountOnly };
  static bool ZPDCensusEnabled();
  void NoteZPDCensusDraw(ZPDCensusDraw kind, bool depth_tested = false);
  void OnZPDQueryResolved(ReportHandle report_handle, uint64_t raw_samples);
  void OpenQuerySegment(bool can_close_submission);
  void CloseQuerySegment();

  // Set by the backend when the draw resolution scale changes.
  uint32_t zpd_draw_resolution_scale_x_ = 1;
  uint32_t zpd_draw_resolution_scale_y_ = 1;
  uint32_t zpd_draw_resolution_scale_x() const { return zpd_draw_resolution_scale_x_; }
  uint32_t zpd_draw_resolution_scale_y() const { return zpd_draw_resolution_scale_y_; }

  std::unordered_map<ReportHandle, ZPDReport> logical_zpd_reports_;
  ActiveZPDSegment zpd_active_segment_{};
  // Census of which draws the occlusion path measured, skipped or could not measure.
  uint32_t zpd_census_measured_ = 0;
  uint32_t zpd_census_unmeasured_ = 0;
  uint32_t zpd_census_skipped_ = 0;
  uint32_t zpd_census_count_only_ = 0;
  uint32_t zpd_census_depth_tested_ = 0;
  // Set when the query pool cannot be used; the fork kept it beside the segment records.
  bool zpd_force_fake_fallback_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
