/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_
#define XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_

#include "rex/graphics/metal/shader_storage_writer.h"
#include "rex/graphics/metal/dxbc_geometry_shader.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include "rex/graphics/metal/dxbc_to_dxil_converter.h"
#include "rex/graphics/metal/metal_fixed_function_state.h"
#include "rex/graphics/metal/metal_dxil_store.h"
#include "rex/graphics/metal/metal_pipeline_storage.h"
#include "rex/graphics/metal/metal_shader.h"
#include "rex/graphics/metal/metal_shader_converter.h"
#include "rex/graphics/shader_storage.h"
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Owns guest-shader loading and the DXBC -> DXIL -> metallib -> MTLFunction
// translation for the Metal backend. This is the first slice of the render
// path (2c): it produces host shader functions but does not build full
// MTLRenderPipelineStates yet (that arrives with the render-target/draw wiring).
class MetalPipelineCache {
 public:
  // device is an id<MTLDevice>, owned by the MetalProvider. The draw
  // resolution scale is baked into translated shaders.
  explicit MetalPipelineCache(void* device,
                              const ::rex::gpu::XenosSettings& settings,
                              uint32_t draw_resolution_scale_x = 1,
                              uint32_t draw_resolution_scale_y = 1);
  ~MetalPipelineCache();

  bool Initialize();
  void Shutdown();

  // Creates (or returns the cached) analyzed shader for the given ucode.
  MetalShader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count);

  // Translates a specific modification of a shader all the way to an
  // MTLFunction (ucode -> DXBC -> DXIL -> metallib -> function). Returns true if
  // the translation produced a usable function. Idempotent per translation.
  bool TranslateShader(MetalShader::MetalTranslation& translation);

  // The single cache-aware DXBC -> DXIL entry point. EVERY production
  // conversion site must use it; calling dxil_converter_raw_.Convert directly
  // silently opts that site out of both the DXIL store and the build census.
  // The key is the exact byte buffer passed in, so any pre-Convert transform
  // (e.g. RelabelDxbcClipDistance) is covered without being enumerated.
  // `site` is a diagnostic label and does not participate in the key.
  bool ConvertDxbcToDxilCached(const std::vector<uint8_t>& dxbc,
                               std::vector<uint8_t>& dxil_out,
                               std::string* error_out, const char* site);

  // Builds (or returns the cached) id<MTLRenderPipelineState> for a translated
  // vertex + pixel function pair and the given attachment formats. The format
  // values are MTLPixelFormat. Returns null on failure. The pixel translation
  // may be null (depth-only). Both translations must already have functions.
  // Vertex-only, rasterization-disabled pipeline for memexport draws (VS
  // eA/eM exports, nothing rasterized, zero attachments). Returns null on
  // failure.
  void* GetOrCreateMemexportPipelineState(MetalShader::MetalTranslation* vertex);

  // Flag bit OR-ed into a `depth_pixel_format` argument: the pass binds the
  // (combined Depth32Float_Stencil8) texture's depth plane only, so the
  // pipeline declares no stencil attachment (reduce-metal-draw-pass-traffic
  // D2: a pass whose draws never test or write stencil leaves the plane
  // unbound and skips its tile load/store). Part of the pipeline key.
  static constexpr uint64_t kDepthWithoutStencilBit = 1ull << 40;

  void* GetOrCreateRenderPipelineState(MetalShader::MetalTranslation* vertex,
                                       MetalShader::MetalTranslation* pixel,
                                       const uint64_t* color_pixel_formats,
                                       uint64_t depth_pixel_format,
                                       MetalPipelineRasterKey raster_key,
                                       const uint32_t* blend_controls,
                                       uint32_t color_write_mask);

  // Background pipeline creation (::rex::graphics::metal::GetMetalSettings().metal_async_pipelines.Get(), default off): a
  // draw whose pipeline is not built yet, and that may use a stand-in, gets
  // kPending while workers translate its shaders and create the state, so a
  // first session does not stop for every new pipeline. Other draws wait for
  // a queued build or build synchronously, as GetOrCreateRenderPipelineState.
  // `vertex` and `pixel` may be untranslated; `pixel` may be null.
  static bool AsyncPipelinesEnabled();
  enum class AsyncPipelineStatus { kReady, kPending, kFailed, kDeferred };
  // kReady: *pipeline_out is the state, *pixel_out the pixel translation it
  // uses (null if the pixel shader did not translate, as the synchronous
  // path does). kFailed: no pipeline (the vertex shader did not translate if
  // it has no function).
  // kDeferred (only with `may_defer`, for a draw that may not use a
  // stand-in): the shaders are translated (*pixel_out as for kReady) and a
  // worker is creating the state, ahead of stand-in builds. *deferred_out
  // owns the request; ResolveDeferredPipeline(deferred_out->get()) blocks
  // until the state exists. The encoder thread resolves it, so the command
  // processor records the next draws (and queues their pipelines) meanwhile:
  // a scene's first frame, whose new pipelines all wait, builds them in
  // parallel instead of one after another.
  AsyncPipelineStatus GetRenderPipelineStateAsync(
      MetalShader::MetalTranslation* vertex,
      MetalShader::MetalTranslation* pixel, const uint64_t* color_pixel_formats,
      uint64_t depth_pixel_format, MetalPipelineRasterKey raster_key,
      const uint32_t* blend_controls, uint32_t color_write_mask,
      bool stand_in_allowed, bool may_defer, void** pipeline_out,
      MetalShader::MetalTranslation** pixel_out,
      std::shared_ptr<void>* deferred_out);
  // A MetalEncodeStream::PipelineResolver: the retained state of a kDeferred
  // request, or null if creation failed. Any thread.
  static void* ResolveDeferredPipeline(void* request);
  // Installs the pipelines the workers finished. GPU thread.
  void DrainAsyncPipelines();

  // Builds (or returns the cached) MESH id<MTLRenderPipelineState> that expands a
  // point/rectangle/quad primitive via MSC geometry emulation: the guest VS runs
  // as the object stage (with a synthesized stage-in), a generated DXBC geometry
  // shader runs as the mesh stage, and `pixel` supplies the fragment stage.
  // `vertex` must already be DXBC-translated (its translated_binary() is the guest
  // VS DXBC, reused as the object stage). On success, returns the pipeline and
  // fills the two out-params with the geometry-pipeline config the draw call needs
  // (VS output size + GS max input primitives per mesh threadgroup). Returns null
  // on failure. Requires mesh-shader support (Apple Silicon); the caller gates on
  // that. The pixel translation may be null (depth-only path unsupported here yet).
  void* GetOrCreateGeometryPipelineState(
      MetalShader::MetalTranslation* vertex,
      MetalShader::MetalTranslation* pixel,
      PipelineGeometryShader geometry_type,
      ::rex::gpu::DXBCShaderTranslator::Modification vertex_modification,
      ::rex::gpu::DXBCShaderTranslator::Modification pixel_modification,
      const uint64_t* color_pixel_formats, uint64_t depth_pixel_format,
      MetalPipelineRasterKey raster_key, const uint32_t* blend_controls,
      uint32_t color_write_mask,
      uint32_t* gs_vertex_size_out, uint32_t* gs_max_input_primitives_out);

  ::rex::gpu::DXBCShaderTranslator* shader_translator() const {
    return shader_translator_.get();
  }

  // Opens the durable pipeline storage for a title (task group 3a). Called from
  // MetalCommandProcessor::InitializeShaderStorage, which the emulator invokes
  // on launch. A no-op unless a durable mechanism is enabled by cvar; failure
  // to open is never fatal — the backend runs exactly as it does today.
  void InitializeDurableStorage(const std::filesystem::path& cache_root,
                                uint32_t title_id);
  void ShutdownDurableStorage();

  // M2 — writes the binary archive if this session added pipelines to it. The
  // emulator quits through std::quick_exit, so nothing runs at teardown: the
  // archive is saved from MetalCommandProcessor::SaveShaderStorageToDisk (GPU
  // paused, just before the exit) and periodically from the swap, which is
  // what keeps a crash or a SIGKILL from losing a whole session of pipelines.
  // `wait` blocks until every queued segment is on disk.
  void SaveBinaryArchive(const char* reason, bool wait);
  void MaybeSaveBinaryArchiveOnSwap();

  const MetalPipelineStorage& durable_storage() const { return storage_; }

  // Shader and pipeline storage: the guest shaders (.xsh, the format D3D12 and
  // Vulkan share) and render pipeline descriptions (.msc.metal.xpso) a title has
  // used, recorded as they are first built and rebuilt at the next launch.
  // The metallib cache and binary archive hold only translator output, so a
  // build that changes the translator discards them; this record is what
  // lets the next launch compile before gameplay instead of inside it.
  // XE_METAL_SHADER_STORAGE=0 disables it. GPU thread only.
  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id);
  void ShutdownShaderStorage();
  // Hands this frame's new records to the writer thread.
  void FlushShaderStorage();

 private:
  // Configured like every translator this cache uses; one per thread that
  // translates (::rex::gpu::DXBCShaderTranslator keeps per-translation state).
  std::unique_ptr<::rex::gpu::DXBCShaderTranslator> CreateShaderTranslator() const;
  // TranslateShader on a given translator and Metal Shader Converter, so the
  // shader storage replay can translate on several threads. Different
  // translations may be translated concurrently; the shaders must already be
  // analyzed and their fetch patterns registered.
  bool TranslateShader(MetalShader::MetalTranslation& translation,
                       ::rex::gpu::DXBCShaderTranslator& translator,
                       MetalShaderConverter& metal_converter);

  // GetOrCreateRenderPipelineState in parts, so the shader storage replay can
  // create states on several threads: Metal state creation is thread-safe,
  // render_pipeline_states_ and the binary archive here are not.
  static uint64_t RenderPipelineKey(MetalShader::MetalTranslation* vertex,
                                    MetalShader::MetalTranslation* pixel,
                                    const uint64_t* color_pixel_formats,
                                    uint64_t depth_pixel_format,
                                    MetalPipelineRasterKey raster_key,
                                    const uint32_t* blend_controls,
                                    uint32_t color_write_mask);
  // A retained MTLRenderPipelineDescriptor. Any thread; `lookup_list` is the
  // binary archive lookup list (NSArray) to search, or null.
  void* NewRenderPipelineDescriptor(MetalShader::MetalTranslation* vertex,
                                    MetalShader::MetalTranslation* pixel,
                                    const uint64_t* color_pixel_formats,
                                    uint64_t depth_pixel_format,
                                    MetalPipelineRasterKey raster_key,
                                    const uint32_t* blend_controls,
                                    uint32_t color_write_mask,
                                    void* lookup_list) const;
  // A retained id<MTLRenderPipelineState>, or null. Any thread, while no
  // other thread changes the archive key set; `archive_stale_out` reports an
  // archived key the archive no longer serves, for the caller to forget.
  void* NewRenderPipelineState(void* descriptor, uint64_t key,
                               bool* archive_stale_out) const;
  // Takes the retained `state` (null caches a failure), adds it to the binary
  // archive and caches it. Returns the cached state.
  void* AddRenderPipelineState(uint64_t key, void* descriptor, void* state);

  // Runs work(worker, item) for every item in [0, item_count), on the calling
  // thread (worker 0) and up to worker_count - 1 more, until all are done.
  static void RunShaderStorageWorkers(
      size_t item_count, uint32_t worker_count,
      const std::function<void(uint32_t worker, size_t item)>& work);

  XEPACKEDSTRUCT(StoredPipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 for a depth-only pipeline.
    uint64_t pixel_shader_hash;
    // With fetch_plain_pattern cleared: the pattern index is session-local,
    // so the pattern itself is stored below and re-registered on load.
    uint64_t pixel_shader_modification;
    uint64_t color_formats[xenos::kMaxColorRenderTargets];
    uint64_t depth_format;
    uint32_t sample_count;
    uint32_t blend_controls[xenos::kMaxColorRenderTargets];
    uint32_t color_write_mask;
    MetalShader::FetchPattern pixel_fetch_pattern;  // fetch_mask 0: none
    static constexpr uint32_t kVersion = 0x20261027;
  });
  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    StoredPipelineDescription description;
  });
  void StorePipeline(MetalShader::MetalTranslation* vertex,
                     MetalShader::MetalTranslation* pixel,
                     const uint64_t* color_pixel_formats,
                     uint64_t depth_pixel_format, uint32_t sample_count,
                     const uint32_t* blend_controls,
                     uint32_t color_write_mask);
  struct AsyncPipelineJob {
    MetalShader::MetalTranslation* vertex;
    MetalShader::MetalTranslation* pixel;
    uint64_t color_formats[xenos::kMaxColorRenderTargets];
    uint64_t depth_format;
    MetalPipelineRasterKey raster_key;
    uint32_t blend_controls[xenos::kMaxColorRenderTargets];
    uint32_t color_write_mask;
    uint64_t async_key;
    void* lookup_list;  // retained NSArray or null
    // Set by the worker.
    bool done = false;
    bool vertex_ok = false;
    uint64_t key = 0;
    MetalShader::MetalTranslation* pixel_used = nullptr;
    void* descriptor = nullptr;  // retained
    void* state = nullptr;       // retained
    // kDeferred requests: translated by the command processor (pixel_used
    // is set before queueing), resolved by the encoder thread. The worker
    // keeps a reference to the state here for the resolver.
    bool deferred = false;
    MetalPipelineCache* cache = nullptr;
    void* deferred_state = nullptr;  // retained
    ~AsyncPipelineJob();
  };
  struct AsyncPipelineResult {
    uint64_t key;  // 0: failed
    MetalShader::MetalTranslation* pixel_used;
  };
  // Content key of a request, valid before the shaders are translated.
  static uint64_t AsyncPipelineKey(MetalShader::MetalTranslation* vertex,
                                   MetalShader::MetalTranslation* pixel,
                                   const uint64_t* color_pixel_formats,
                                   uint64_t depth_pixel_format,
                                   MetalPipelineRasterKey raster_key,
                                   const uint32_t* blend_controls,
                                   uint32_t color_write_mask);
  void StartAsyncPipelineWorkers();
  void StopAsyncPipelineWorkers();
  void AsyncPipelineWorker();
  std::mutex async_mutex_;
  std::condition_variable async_queue_cv_;
  std::condition_variable async_done_cv_;
  std::deque<std::shared_ptr<AsyncPipelineJob>> async_queue_;
  std::vector<std::shared_ptr<AsyncPipelineJob>> async_done_;
  // Set with async_done_ (under async_mutex_) so DrainAsyncPipelines, called
  // for every draw, skips the lock while no build has finished.
  std::atomic<bool> async_done_pending_{false};
  bool async_stop_ = false;
  // GPU thread only.
  std::unordered_map<uint64_t, std::shared_ptr<AsyncPipelineJob>>
      async_pending_;
  std::unordered_map<uint64_t, AsyncPipelineResult> async_results_;
  // The last ready async_results_ lookup, by raw inputs: consecutive draws
  // mostly repeat the pipeline, and this skips the key hash and both map
  // lookups (~0.3 ms/frame of the CP thread in GTA IV on the Mac Pro).
  // Translations live until Shutdown, so their pointers identify them.
  struct AsyncPipelineMemo {
    MetalShader::MetalTranslation* vertex = nullptr;
    MetalShader::MetalTranslation* pixel = nullptr;
    uint64_t colors[xenos::kMaxColorRenderTargets] = {};
    uint64_t depth = 0;
    uint32_t sample_count = 0;
    uint32_t blends[xenos::kMaxColorRenderTargets] = {};
    uint32_t write_mask = 0;
    void* pipeline = nullptr;
    MetalShader::MetalTranslation* pixel_used = nullptr;
  };
  AsyncPipelineMemo async_memo_;
  std::vector<std::unique_ptr<rex::thread::Thread>> async_workers_;

  ShaderStorageWriter<PipelineStoredDescription> shader_storage_;
  bool shader_storage_flush_shaders_ = false;
  bool shader_storage_flush_pipelines_ = false;
  // Set while stored pipelines are rebuilt, so they are not written again.
  bool shader_storage_replaying_ = false;

  // A guest VS compiled as an MSC geometry-emulation OBJECT stage, plus its
  // synthesized stage-in. Metal objects held as retained void*.
  struct GeometryVertexStage {
    void* library = nullptr;           // id<MTLLibrary> (object stage).
    void* stage_in_library = nullptr;  // id<MTLLibrary> (stage-in).
    std::string function_name;         // base name; MSC appends the object suffix.
    uint32_t vertex_output_size = 0;   // -> IRRuntimeGeometryPipelineConfig.
    bool valid = false;
  };
  // A generated DXBC geometry shader compiled as an MSC MESH stage.
  struct GeometryShaderStage {
    void* library = nullptr;  // id<MTLLibrary> (mesh stage).
    std::string function_name;
    uint32_t max_input_primitives = 0;  // -> IRRuntimeGeometryPipelineConfig.
    bool valid = false;
  };
  // An assembled geometry-emulation mesh pipeline + the config the draw needs.
  struct GeometryPipeline {
    void* pipeline = nullptr;  // id<MTLRenderPipelineState> (mesh), retained.
    uint32_t gs_vertex_size = 0;
    uint32_t gs_max_input_primitives = 0;
    // Draws suppressed by a cached FAILED creation (pipeline == nullptr). Two
    // failed creations can suppress many later draws; this is the impact
    // number, counted separately from creation attempts.
    uint32_t negative_hits = 0;
  };

  // Object-stage VS for geometry emulation, cached by the vertex translation.
  const GeometryVertexStage* GetOrCreateGeometryVertexStage(
      MetalShader::MetalTranslation* vertex,
      PipelineGeometryShader geometry_type);
  // Mesh-stage generated GS, cached by the geometry-shader key.
  const GeometryShaderStage* GetOrCreateGeometryShaderStage(GeometryShaderKey key);
  // Wraps a metallib blob in an id<MTLLibrary> (retained void*); null on failure.
  void* NewLibraryFromBytes(const std::vector<uint8_t>& bytes, const char* label);

  void OpenBinaryArchive();
  void CloseBinaryArchive();
  void RebuildBinaryArchiveLookupList();
  void FinishBinaryArchiveCompaction();

  void* device_;  // id<MTLDevice>, not owned.
  const ::rex::gpu::XenosSettings& xenos_settings_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  std::unique_ptr<::rex::gpu::DXBCShaderTranslator> shader_translator_;
  // NAMED to make a bypass visible. Only ConvertDxbcToDxilCached may call this
  // directly; every other site must go through it or it silently opts out of
  // both the DXIL store and the build census — which is exactly what six
  // production sites had done. `grep -c "dxil_converter_raw_\.Convert"` must
  // stay at 2 (the cold conversion and the verification re-convert, both inside
  // the helper).
  DxbcToDxilConverter dxil_converter_raw_;
  MetalShaderConverter metal_converter_;

  // The metallib store and the DXIL store, written by every thread that
  // translates.
  std::mutex durable_store_mutex_;

  // Owns the loaded shaders, keyed by ucode hash.
  std::unordered_map<uint64_t, MetalShader*> shaders_;

  // Owns id<MTLRenderPipelineState> (retained), keyed by function+format hash.
  // A null value caches a known-failed combination.
  std::unordered_map<uint64_t, void*> render_pipeline_states_;

  // Geometry-emulation caches: object-stage VS by vertex translation, mesh-stage
  // GS by GeometryShaderKey, assembled pipeline by the full pipeline key. The
  // libraries are owned (retained) here and released in Shutdown.
  std::unordered_map<MetalGeometryVertexStageKey, GeometryVertexStage,
                     MetalGeometryVertexStageKey::Hasher>
      geometry_vertex_stages_;
  std::unordered_map<uint32_t, GeometryShaderStage> geometry_shader_stages_;
  std::unordered_map<uint64_t, GeometryPipeline> geometry_pipelines_;

  // Durable on-disk storage for M1/M2. Closed means "no durable cache", which
  // is today's behavior and the default.
  MetalPipelineStorage storage_;

  // M2 — the binary archive, null unless ::rex::graphics::metal::GetMetalSettings().metal_pipeline_binary_archive.Get() is on
  // and storage_ opened. Only the thread that creates render pipelines touches
  // it, plus SaveShaderStorageToDisk with that thread paused. Segments
  // (id<MTLBinaryArchive>, retained) are loaded or already serialized and are
  // only read; `binary_archive_` is the fresh archive new pipelines go to, and
  // `binary_archive_lookup_list_` (NSArray, retained) is the LOADED segments
  // for MTLRenderPipelineDescriptor.binaryArchives (archives this session
  // made only hold pipelines already in render_pipeline_states_). `archived_pipeline_keys_` are
  // the keys in any of them, so a pipeline is added once ever and a lookup can
  // fail fast on a miss; `binary_archive_pending_keys_` are the ones only in
  // the fresh archive.
  std::vector<void*> binary_archive_segments_;
  void* binary_archive_ = nullptr;
  void* binary_archive_lookup_list_ = nullptr;
  std::unordered_set<uint64_t> archived_pipeline_keys_;
  std::vector<uint64_t> binary_archive_pending_keys_;
  uint32_t binary_archive_next_segment_ = 0;
  // Serial dispatch queue (retained) that serializes finished archives.
  void* binary_archive_queue_ = nullptr;
  std::chrono::steady_clock::time_point binary_archive_last_save_;
  // Compaction. A session that opens with more than kCompactArchiveSegments
  // segments adds every pipeline it obtains, archive hits included, to the
  // fresh archive again (`binary_archive_session_keys_`). At quit it deletes
  // every old segment if 80% of their pipelines were saved again, otherwise
  // only the segments whose pipelines all were.
  struct OldArchiveSegment {
    std::filesystem::path path;
    std::vector<uint64_t> keys;
  };
  std::vector<OldArchiveSegment> binary_archive_compacting_;
  std::unordered_set<uint64_t> binary_archive_session_keys_;
  uint64_t binary_archive_failed_at_open_ = 0;

  // Durable DXIL-intermediate store (task 2.2). SEPARATE from storage_ and
  // deliberately so: its identity omits the MSC hash, the device and the OS
  // build, none of which a DXIL artifact depends on. That is what lets it
  // survive an OS or MSC change that legitimately discards every metallib.
  // Default off (cvar ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache.Get()) — task 1.1 measured that it saves
  // nothing on an ordinary warm launch.
  MetalDxilStore dxil_store_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_
