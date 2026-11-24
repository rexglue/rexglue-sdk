/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_pipeline_cache.h"
#include "rex/graphics/metal/metal_draw_resolution_scale.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xxhash.h"
#include "rex/cvar.h"
#include <rex/logging/log_category.h>
#include "rex/string_buffer.h"
#include "rex/graphics/metal/metal_dxbc_clip_relabel.h"
#include "rex/graphics/metal/metal_edram_authority.h"
#include "rex/graphics/metal/dxbc_geometry_shader.h"
#include "rex/graphics/metal/metal_artifact_hit_verify.h"
#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_shader_build_census.h"
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>
#include "rex/ui/graphics_provider.h"

// Metal Shader Converter runtime (declarations only; implementation TU is
// ir_runtime_impl.mm). Provides IRGeometryEmulationPipelineDescriptor and
// IRRuntimeNewGeometryEmulationPipeline for the geometry-emulation mesh pipeline,
#include "metal_irconverter_runtime.h"


// Defined in graphics_system.cc. Gates GraphicsSystem::InitializeShaderStorage
// before any backend is consulted, so it silently disables both durable
// mechanisms; reported in the effective-state line below.


namespace rex {
namespace graphics {
namespace metal {


MetalPipelineCache::MetalPipelineCache(void* device, const ::rex::gpu::XenosSettings& settings,
                                       uint32_t draw_resolution_scale_x,
                                       uint32_t draw_resolution_scale_y)
    : device_(device),
      xenos_settings_(settings),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y) {}

MetalPipelineCache::~MetalPipelineCache() { Shutdown(); }

bool MetalPipelineCache::Initialize() {
  // Match the shader-spike configuration: Apple vendor, bindless resources, no
  // pixel-shader-interlock EDRAM (the copy-based RT model), which is what the
  // Xbox-360 root signature in MetalShaderConverter expects.
  // gamma_render_target_as_unorm8=true: k_8_8_8_8_GAMMA render targets are
  // plain unorm8 on our side, so the PS must PWL-gamma-encode its linear output
  // when kSysFlag_ConvertColorNToGamma is set. Without this ctor flag the
  // translator never emits that codepath and the flag is silently ignored —
  // JC2's scene renders ~0.02-linear sunlight that only becomes visible
  // (0x5C-range) after gamma encoding + exposure (the "black gameplay" bug).
  // Configuration comes from MetalTranslatorConfig so the durable cache's
  // schema fingerprint (3.3b) cannot drift from what is actually built here.
  shader_translator_ = CreateShaderTranslator();

  std::string error;
  if (!dxil_converter_raw_.Initialize(&error)) {
    REXLOG_ERROR("MetalPipelineCache: DXBC->DXIL converter init failed: {}", error);
    return false;
  }
  if (!metal_converter_.Initialize(
          MetalTranslatorConfig::kMscBindlessResourcesUsed, &error)) {
    REXLOG_ERROR("MetalPipelineCache: Metal Shader Converter init failed: {}", error);
    return false;
  }

  // EFFECTIVE durable-cache state, logged unconditionally so a gate can assert
  // on it instead of trusting that a flag was applied.
  //
  // Two ways a requested mode silently fails to take effect, both hit in
  // practice: `macXenos` reads a persisted config that OVERRIDES
  // command-line cvars (while the trace-dump tool has no config, so there the
  // command line wins), and `store_shaders=false` makes
  // GraphicsSystem::InitializeShaderStorage return before any backend is
  // consulted, disabling BOTH mechanisms no matter what their own switches say.
  // Either one produces a run that looks like the mechanism is on and measures
  // as though it is off.
  REXLOG_INFO(
      "MetalPipelineCache: durable cache EFFECTIVE state — M1 metallib={}, "
      "M2 archive={}, store_shaders={}{}",
      ::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() ? "on" : "off",
      ::rex::graphics::metal::GetMetalSettings().metal_pipeline_binary_archive.Get() ? "on" : "off",
      ::rex::graphics::metal::GetMetalSettings().store_shaders.Get() ? "on" : "off",
      ::rex::graphics::metal::GetMetalSettings().store_shaders.Get()
          ? ""
          : "  <- store_shaders=off DISABLES BOTH regardless of the switches");
  return true;
}

std::unique_ptr<::rex::gpu::DXBCShaderTranslator>
MetalPipelineCache::CreateShaderTranslator() const {
  auto translator = std::make_unique<::rex::gpu::DXBCShaderTranslator>(
      xenos_settings_, ::rex::gpu::DXBCOptions{},
      ::rex::gpu::GraphicsProvider::GPUVendorID::kApple,
      MetalTranslatorConfig::kBindlessResourcesUsed,
      MetalTranslatorConfig::kEdramRovUsed,
      MetalTranslatorConfig::GammaRenderTargetAsUnorm8(),
      /*msaa_2x_supported=*/true, draw_resolution_scale_x_,
      draw_resolution_scale_y_);
  // Both of these select behaviour the fork patched into its own translator: an exact host-pixel position at a draw resolution
  // scale, and a deferred kill for non-Apple GPUs. Upstream's DXBCShaderTranslator implements neither, so the settings are read
  // and reported rather than silently ignored. Porting the two translator changes is separate work, tracked in the plan.
  static const bool s_reported = [] {
    REXLOG_INFO("MetalPipelineCache: exact scaled pixel position {}, deferred kill {} (neither is implemented by this tree's translator)",
                MetalExactScaledPixelPositionEnabled() ? "requested" : "off",
                MetalDeferredKillEnabled() ? "requested" : "off");
    return true;
  }();
  (void)s_reported;
  return translator;
}

void MetalPipelineCache::InitializeDurableStorage(
    const std::filesystem::path& cache_root, uint32_t title_id) {
  ShutdownDurableStorage();
  // 3.1 — exact rollback switches, default OFF. Neither mechanism consumes the
  // storage yet (that is 3b/3c); opening it here is what makes the identity,
  // bounds, and locking path live rather than dormant.
  if (!::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() &&
      !::rex::graphics::metal::GetMetalSettings().metal_pipeline_binary_archive.Get()) {
    return;
  }
  MetalPipelineStorageIdentity identity = ComputeMetalPipelineStorageIdentity(
      device_, draw_resolution_scale_x_, draw_resolution_scale_y_);
  REXLOG_INFO("MetalDurability: cvars metallib={} archive={} cache_root='{}' title {:08X}",
              ::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get(), ::rex::graphics::metal::GetMetalSettings().metal_pipeline_binary_archive.Get(),
              cache_root.string(), title_id);
  if (!storage_.Open(cache_root, title_id, identity)) {
    REXLOG_WARN(
        "MetalPipelineCache: durable storage unavailable for title {:08X}; "
        "continuing with in-memory caches only",
        title_id);
  }
  OpenBinaryArchive();

  // Task 2.2 — the DXIL store, opened independently and with its own identity.
  // Note it is NOT under the title directory and its Open is not conditional on
  // the pipeline storage having opened: the whole point is that the two
  // invalidate on different facts.
  // EFFECTIVE-STATE LOG. Emitted in BOTH arms and greppable (MetalDxilGate), so
  // "this run was default-on" and "this run was the rollback" are provable from
  // the log rather than assumed from the command line — a persisted config
  // value silently overriding the code default is exactly how the promotion
  // measurement could have been wrong (the config had ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache.Get()=false
  // written into it while the flag was default-off).
  REXLOG_INFO(
      "MetalDxilGate: ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache.Get()={} (default-on since 2026-08-05; false "
      "is the exact rollback), verify_every_nth={}, cap={} MiB",
      ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache.Get() ? "TRUE" : "FALSE",
      ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache_verify_every_nth.Get(),
      ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache_cap_mib.Get());
  if (::rex::graphics::metal::GetMetalSettings().metal_dxil_cache.Get()) {
    if (!dxil_store_.Open(cache_root, ComputeMetalDxilStoreIdentity())) {
      REXLOG_WARN(
          "MetalDxilGate: store unavailable; DXBC->DXIL runs cold this session");
    }
  } else {
    REXLOG_INFO(
        "MetalDxilGate: ROLLBACK ARM — no DXIL store opened, no durable reads "
        "or writes this session");
  }
}

void MetalPipelineCache::ShutdownDurableStorage() {
  SaveBinaryArchive("shutdown", true);
  CloseBinaryArchive();
  storage_.Close();
  dxil_store_.Close();
}

namespace {
// M2 storage layout: <title>/archive/NNNNNNNN.metalar segments, each with an
// NNNNNNNN.keys sidecar listing the pipeline keys it holds. The archive cannot
// be enumerated, and without the keys every session would re-add every
// pipeline (one add is a host compile when the macOS cache is cold).
//
// WHY SEGMENTS. An MTLBinaryArchive serializes reliably exactly once. Loading
// one and adding to it, or serializing one a second time after more adds,
// fails in the packer ("Failed to generate machO ... expecting 'fragment'
// stage in pipeline no. N", Mafia II on M1 Max, macOS 27) and leaves the
// in-memory archive empty. So loaded segments are only ever read, new
// pipelines go to a fresh archive, and every save serializes that fresh archive
// once as a new segment and starts another.
constexpr uint32_t kArchiveKeysMagic = 0x4B415845;  // 'EXAK'
constexpr uint32_t kArchiveKeysVersion = 1;
// Segments accumulate one per save that had new pipelines, which once a title
// is warm is about one small segment per session. Past the compaction count a
// session compacts (FinishBinaryArchiveCompaction); kMaxArchiveSegments is the
// backstop if compaction keeps finding too little reuse, and rebuilds.
constexpr size_t kCompactArchiveSegments = 16;
constexpr size_t kMaxArchiveSegments = 64;

size_t CompactArchiveSegments() {
  static const size_t count = [] {
    const char* v = std::getenv("XE_METAL_ARCHIVE_COMPACT_SEGMENTS");
    return v ? size_t(std::strtoull(v, nullptr, 10)) : kCompactArchiveSegments;
  }();
  return count;
}

bool ReadArchiveKeys(const std::filesystem::path& path,
                     std::vector<uint64_t>* keys_out) {
  keys_out->clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  uint32_t header[3] = {};
  bool ok = std::fread(header, sizeof(header), 1, f) == 1 &&
            header[0] == kArchiveKeysMagic && header[1] == kArchiveKeysVersion;
  if (ok) {
    keys_out->resize(header[2]);
    ok = keys_out->empty() ||
         std::fread(keys_out->data(), sizeof(uint64_t), keys_out->size(), f) ==
             keys_out->size();
  }
  std::fclose(f);
  return ok;
}

id<MTLBinaryArchive> NewEmptyBinaryArchive(id<MTLDevice> device) {
  MTLBinaryArchiveDescriptor* desc = [[MTLBinaryArchiveDescriptor alloc] init];
  NSError* error = nil;
  id<MTLBinaryArchive> archive =
      [device newBinaryArchiveWithDescriptor:desc error:&error];
  if (!archive) {
    REXLOG_WARN("MetalPipelineCache: binary archive unavailable: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
  }
  return archive;
}
}  // namespace

void MetalPipelineCache::RebuildBinaryArchiveLookupList() {
  NSMutableArray<id<MTLBinaryArchive>>* list = [NSMutableArray array];
  for (void* segment : binary_archive_segments_) {
    [list addObject:(__bridge id<MTLBinaryArchive>)segment];
  }
  if (binary_archive_lookup_list_) {
    NSArray* old = (__bridge_transfer NSArray*)binary_archive_lookup_list_;
    (void)old;
  }
  binary_archive_lookup_list_ = (__bridge_retained void*)[list copy];
}

void MetalPipelineCache::OpenBinaryArchive() {
  CloseBinaryArchive();
  GetMetalShaderBuildCensus().archive_enabled = false;
  if (!::rex::graphics::metal::GetMetalSettings().metal_pipeline_binary_archive.Get() || !storage_.is_open() ||
      !device_) {
    return;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  const std::filesystem::path dir = storage_.binary_archive_path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  // Segments in sequence order; a segment without readable keys (a crash
  // between the archive rename and the keys write) is an orphan and removed.
  std::vector<std::pair<uint32_t, std::filesystem::path>> segments;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::filesystem::path& path = entry.path();
    if (path.extension() == ".tmp") {
      std::filesystem::remove(path, ec);
      continue;
    }
    if (path.extension() != ".metalar") {
      continue;
    }
    char* end = nullptr;
    const std::string stem = path.stem().string();
    const unsigned long seq = std::strtoul(stem.c_str(), &end, 10);
    if (end && *end == '\0') {
      segments.emplace_back(uint32_t(seq), path);
    }
  }
  std::sort(segments.begin(), segments.end());
  if (segments.size() > kMaxArchiveSegments) {
    REXLOG_INFO(
        "MetalPipelineCache: {} binary archive segments exceed {}; rebuilding",
        segments.size(), kMaxArchiveSegments);
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    segments.clear();
  }

  const bool compact = segments.size() > CompactArchiveSegments();
  std::vector<uint64_t> keys;
  for (const auto& [seq, path] : segments) {
    binary_archive_next_segment_ = seq + 1;
    std::filesystem::path keys_path = path;
    keys_path.replace_extension(".keys");
    if (!ReadArchiveKeys(keys_path, &keys)) {
      std::filesystem::remove(path, ec);
      std::filesystem::remove(keys_path, ec);
      continue;
    }
    MTLBinaryArchiveDescriptor* desc = [[MTLBinaryArchiveDescriptor alloc] init];
    desc.url =
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSError* error = nil;
    id<MTLBinaryArchive> segment =
        [device newBinaryArchiveWithDescriptor:desc error:&error];
    if (!segment) {
      REXLOG_WARN("MetalPipelineCache: binary archive segment {} failed to load "
             "({}); removed",
             path.filename().string(),
             error ? [[error localizedDescription] UTF8String] : "unknown");
      std::filesystem::remove(path, ec);
      std::filesystem::remove(keys_path, ec);
      continue;
    }
    binary_archive_segments_.push_back((__bridge_retained void*)segment);
    archived_pipeline_keys_.insert(keys.begin(), keys.end());
    if (compact) {
      binary_archive_compacting_.push_back({path, keys});
    }
  }

  id<MTLBinaryArchive> fresh = NewEmptyBinaryArchive(device);
  if (!fresh) {
    CloseBinaryArchive();
    return;
  }
  binary_archive_ = (__bridge_retained void*)fresh;
  if (!binary_archive_segments_.empty()) {
    RebuildBinaryArchiveLookupList();
  }
  if (!binary_archive_queue_) {
    binary_archive_queue_ = (__bridge_retained void*)dispatch_queue_create(
        "xe.metal.binary_archive_save", DISPATCH_QUEUE_SERIAL);
  }
  binary_archive_last_save_ = std::chrono::steady_clock::now();
  binary_archive_failed_at_open_ =
      GetMetalShaderBuildCensus().archive_save_failed.load(
          std::memory_order_relaxed);
  GetMetalShaderBuildCensus().archive_enabled = true;
  REXLOG_INFO("MetalPipelineCache: binary archive: {} segments, {} pipeline keys{}",
         binary_archive_segments_.size(), archived_pipeline_keys_.size(),
         binary_archive_compacting_.empty() ? "" : ", compacting");
}

void MetalPipelineCache::CloseBinaryArchive() {
  if (binary_archive_queue_) {
    dispatch_sync((__bridge dispatch_queue_t)binary_archive_queue_, ^{
                  });
  }
  for (void* segment : binary_archive_segments_) {
    id<MTLBinaryArchive> archive = (__bridge_transfer id<MTLBinaryArchive>)segment;
    (void)archive;
  }
  binary_archive_segments_.clear();
  if (binary_archive_) {
    id<MTLBinaryArchive> archive =
        (__bridge_transfer id<MTLBinaryArchive>)binary_archive_;
    (void)archive;
    binary_archive_ = nullptr;
  }
  if (binary_archive_lookup_list_) {
    NSArray* list = (__bridge_transfer NSArray*)binary_archive_lookup_list_;
    (void)list;
    binary_archive_lookup_list_ = nullptr;
  }
  archived_pipeline_keys_.clear();
  binary_archive_pending_keys_.clear();
  binary_archive_next_segment_ = 0;
  binary_archive_compacting_.clear();
  binary_archive_session_keys_.clear();
}

void MetalPipelineCache::SaveBinaryArchive(const char* reason, bool wait) {
  if (binary_archive_ && !binary_archive_pending_keys_.empty() &&
      storage_.is_open() && device_ && binary_archive_queue_) {
    // Hand the fresh archive to the save queue and start another. Nothing on
    // this thread reads it again: its pipelines are already in
    // render_pipeline_states_, so no lookup this session can need it, and the
    // serialize (476 ms for 222 pipelines on the M1 Max) stays off the frame.
    id<MTLBinaryArchive> archive =
        (__bridge_transfer id<MTLBinaryArchive>)binary_archive_;
    binary_archive_ = nullptr;
    std::vector<uint64_t> keys = std::move(binary_archive_pending_keys_);
    binary_archive_pending_keys_.clear();
    const uint32_t segment = binary_archive_next_segment_++;
    const std::filesystem::path path =
        storage_.binary_archive_path() / fmt::format("{:08}.metalar", segment);
    const std::string reason_copy = reason ? reason : "";
    dispatch_async(
        (__bridge dispatch_queue_t)binary_archive_queue_, ^{
          const auto start = std::chrono::steady_clock::now();
          std::filesystem::path keys_path = path;
          keys_path.replace_extension(".keys");
          std::filesystem::path temp_path = path;
          temp_path += ".tmp";
          std::error_code ec;
          std::filesystem::remove(temp_path, ec);
          NSError* error = nil;
          NSURL* temp_url = [NSURL
              fileURLWithPath:[NSString stringWithUTF8String:temp_path.c_str()]];
          bool saved = [archive serializeToURL:temp_url error:&error];
          if (saved) {
            std::filesystem::rename(temp_path, path, ec);
            saved = !ec;
          }
          MetalShaderBuildCensus& census = GetMetalShaderBuildCensus();
          if (!saved) {
            // Those pipelines are simply not on disk; the next session finds
            // their keys in no sidecar and adds them again.
            REXLOG_WARN("MetalPipelineCache: binary archive save ({}) failed: {}",
                   reason_copy,
                   ec ? ec.message()
                      : (error ? [[error localizedDescription] UTF8String]
                               : "unknown"));
            std::filesystem::remove(temp_path, ec);
            census.archive_save_failed.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          // The sidecar goes after the archive, so keys never claim pipelines
          // a segment lacks; a segment without one is an orphan at open.
          std::vector<uint8_t> blob(sizeof(uint32_t) * 3 +
                                    sizeof(uint64_t) * keys.size());
          const uint32_t header[3] = {kArchiveKeysMagic, kArchiveKeysVersion,
                                      uint32_t(keys.size())};
          std::memcpy(blob.data(), header, sizeof(header));
          std::memcpy(blob.data() + sizeof(header), keys.data(),
                      sizeof(uint64_t) * keys.size());
          MetalPipelineStorage::WriteFileAtomic(keys_path, blob.data(),
                                                blob.size());
          const uint64_t bytes = std::filesystem::file_size(path, ec);
          // Off the frame thread, so Stage::Add rather than RecordBuild: this
          // is not stall time.
          census.archive_save.Add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count()),
              ec ? 0 : bytes);
          REXLOG_INFO(
              "MetalPipelineCache: binary archive saved ({}): segment {} with "
              "{} pipelines, {} KiB",
              reason_copy, segment, keys.size(), (ec ? 0 : bytes) / 1024);
        });
    id<MTLBinaryArchive> fresh =
        NewEmptyBinaryArchive((__bridge id<MTLDevice>)device_);
    if (fresh) {
      binary_archive_ = (__bridge_retained void*)fresh;
    }
    binary_archive_last_save_ = std::chrono::steady_clock::now();
  }
  if (wait && binary_archive_queue_) {
    dispatch_sync((__bridge dispatch_queue_t)binary_archive_queue_, ^{
                  });
    FinishBinaryArchiveCompaction();
  }
}

void MetalPipelineCache::FinishBinaryArchiveCompaction() {
  // Only once everything this session added is on disk: a failed save means
  // some of the keys counted below are in no segment.
  MetalShaderBuildCensus& census = GetMetalShaderBuildCensus();
  if (binary_archive_compacting_.empty() ||
      !binary_archive_pending_keys_.empty() ||
      census.archive_save_failed.load(std::memory_order_relaxed) !=
          binary_archive_failed_at_open_) {
    return;
  }
  // A session rarely touches every archived pipeline, and the few it skips
  // are spread across the old segments, so requiring full coverage per
  // segment removes almost nothing (M1 Max Mafia II: 375 of 415 re-saved, 1 of
  // 5 segments removable). Once 80% are re-saved, all old segments go: the
  // rest were unused this session and are rebuilt and re-archived if a later
  // one needs them.
  std::unordered_set<uint64_t> old_keys;
  for (const OldArchiveSegment& old : binary_archive_compacting_) {
    old_keys.insert(old.keys.begin(), old.keys.end());
  }
  size_t resaved = 0;
  for (uint64_t key : old_keys) {
    resaved += binary_archive_session_keys_.count(key);
  }
  const bool remove_all = resaved * 5 >= old_keys.size() * 4;
  size_t removed = 0;
  std::error_code ec;
  for (const OldArchiveSegment& old : binary_archive_compacting_) {
    if (!remove_all &&
        !std::all_of(old.keys.begin(), old.keys.end(), [this](uint64_t key) {
          return binary_archive_session_keys_.count(key) != 0;
        })) {
      continue;
    }
    // Sidecar first: a segment left without one is an orphan the next open
    // removes. The loaded archive keeps its mapping until it is released.
    std::filesystem::path keys_path = old.path;
    keys_path.replace_extension(".keys");
    std::filesystem::remove(keys_path, ec);
    std::filesystem::remove(old.path, ec);
    ++removed;
  }
  REXLOG_INFO("MetalPipelineCache: binary archive compacted: {} of {} old segments "
         "removed ({} of {} old pipelines saved again)",
         removed, binary_archive_compacting_.size(), resaved, old_keys.size());
  binary_archive_compacting_.clear();
}

void MetalPipelineCache::MaybeSaveBinaryArchiveOnSwap() {
  // Every save is a new segment, so saves are rare: often enough that a crash
  // loses at most a few minutes of new pipelines, and only once enough have
  // accumulated to be worth a segment. Quit saves whatever is left.
  if (!binary_archive_ || binary_archive_pending_keys_.size() < 16) {
    return;
  }
  static const int64_t s_interval_s = [] {
    const char* v = std::getenv("XE_METAL_ARCHIVE_SAVE_INTERVAL_S");
    return v ? int64_t(std::strtoll(v, nullptr, 10)) : int64_t(300);
  }();
  if (std::chrono::steady_clock::now() - binary_archive_last_save_ <
      std::chrono::seconds(s_interval_s)) {
    return;
  }
  SaveBinaryArchive("periodic", false);
}

void MetalPipelineCache::Shutdown() {
  StopAsyncPipelineWorkers();
  ShutdownDurableStorage();
  if (binary_archive_queue_) {
    dispatch_queue_t queue =
        (__bridge_transfer dispatch_queue_t)binary_archive_queue_;
    (void)queue;
    binary_archive_queue_ = nullptr;
  }
  for (auto& pair : render_pipeline_states_) {
    if (pair.second) {
      id<MTLRenderPipelineState> state =
          (__bridge_transfer id<MTLRenderPipelineState>)pair.second;
      (void)state;
    }
  }
  render_pipeline_states_.clear();
  async_memo_ = {};
  // Release geometry-emulation pipelines + stage libraries (all retained void*).
  for (auto& pair : geometry_pipelines_) {
    if (pair.second.pipeline) {
      id<MTLRenderPipelineState> state =
          (__bridge_transfer id<MTLRenderPipelineState>)pair.second.pipeline;
      (void)state;
    }
  }
  geometry_pipelines_.clear();
  for (auto& pair : geometry_vertex_stages_) {
    if (pair.second.library) {
      id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)pair.second.library;
      (void)lib;
    }
    if (pair.second.stage_in_library) {
      id<MTLLibrary> lib =
          (__bridge_transfer id<MTLLibrary>)pair.second.stage_in_library;
      (void)lib;
    }
  }
  geometry_vertex_stages_.clear();
  for (auto& pair : geometry_shader_stages_) {
    if (pair.second.library) {
      id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)pair.second.library;
      (void)lib;
    }
  }
  geometry_shader_stages_.clear();
  for (auto& pair : shaders_) {
    delete pair.second;
  }
  shaders_.clear();
  shader_translator_.reset();
}

MetalShader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address,
                                            uint32_t dword_count) {
  uint64_t data_hash =
      XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second;
  }
  // Create and stash the shader even if it later fails translation, so we don't
  // retry a known-bad shader.
  MetalShader* shader =
      new MetalShader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

namespace {
// M1 entry key. Same content-derived identity the pipeline key uses, hashed to
// a filename. The storage-wide provenance (device, OS, translator artifacts and
// schema) was validated when the directory was opened, so it is not repeated
// per entry.
uint64_t MetallibCacheKey(const MetalShader::MetalTranslation& translation) {
  struct KeyBytes {
    uint64_t ucode_hash;
    uint64_t modification;
    uint32_t shader_type;
    uint32_t pad;
    uint64_t fetch_pattern;
  } key_bytes = {};
  key_bytes.ucode_hash = translation.GetShader().GetUcodeDataHash();
  key_bytes.modification = translation.GetModification();
  key_bytes.shader_type = uint32_t(translation.GetShader().GetType());
  // The pixel fetch pattern index is session-local; key on its content.
  if (translation.GetShader().GetType() == xenos::ShaderType::kPixel) {
    ::rex::gpu::DXBCShaderTranslator::Modification modification(
        translation.GetModification());
    key_bytes.fetch_pattern =
        static_cast<const MetalShader&>(translation.GetShader())
            .FetchPatternContentHash(modification.pixel.fetch_plain_pattern);
    modification.pixel.fetch_plain_pattern = 0;
    key_bytes.modification = modification.value;
  }
  return XXH3_64bits(&key_bytes, sizeof(key_bytes));
}

}  // namespace


// The ONE place DXBC becomes DXIL. Every production conversion site routes here
// (adopt-xenios-shader-artifact-caching, scope correction 2026-08-05): before
// this existed the cache covered only the ordinary vertex/pixel path while the
// geometry object stage, the generated geometry stage, the depth-only fragment
// stage and all three tessellation stages called dxilconv directly. Geometry
// emulation is DEFAULT production behavior, so the majority of a
// geometry-heavy title's conversions bypassed both the cache and the census —
// which is also why the task 1.1 dxilconv figures UNDERSTATE the real cost.
//
// THE KEY IS THE EXACT BYTE BUFFER HANDED TO dxilconv, never a shader
// identity. That is not a convenience: the geometry sites mutate the DXBC with
// RelabelDxbcClipDistance immediately before converting, and a shader-level key
// cannot see that mutation. Keying on the post-transform bytes makes every
// present and future pre-Convert transform automatically covered — the same
// class of blindness that made kFetchAbiRevision 4->5 necessary.
//
// `site` names the caller for diagnostics only; it deliberately does NOT
// participate in the key, because two sites converting identical bytes must
// share the artifact (the prebuilt tessellation blobs are identical across
// every title, and 8 of Sonic's 94 conversions were duplicate DXBC).
bool MetalPipelineCache::ConvertDxbcToDxilCached(
    const std::vector<uint8_t>& dxbc, std::vector<uint8_t>& dxil_out,
    std::string* error_out, const char* site) {
  MetalShaderBuildCensus& census = GetMetalShaderBuildCensus();
  const bool cache_enabled =
      ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache.Get() && dxil_store_.is_open();
  const MetalDxilArtifactKey key = MetalDxilArtifactKey::ForDxbc(dxbc);

  if (cache_enabled) {
    std::vector<uint8_t> cached;
    const auto load_start = std::chrono::steady_clock::now();
    MetalDxilLoadResult result;
    {
      std::lock_guard<std::mutex> lock(durable_store_mutex_);
      result = dxil_store_.Load(key, &cached);
    }
    census.dxil_load_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - load_start)
                     .count()),
        std::memory_order_relaxed);
    if (result == MetalDxilLoadResult::kRejectedCorrupt) {
      // Not a miss. The store already logged and deleted the entry; the cold
      // conversion below rebuilds it. Counted as a correctness incident.
      census.dxil_cache_corrupt.fetch_add(1, std::memory_order_relaxed);
    } else if (result == MetalDxilLoadResult::kHit) {
      const uint64_t hit_index =
          census.dxil_cache_hits.fetch_add(1, std::memory_order_relaxed) + 1;
      // SAMPLED HIT VERIFICATION (2.3, invariant from 2.1): a hit must equal a
      // fresh conversion byte for byte, or it must miss. Sampled rather than
      // universal because verifying every hit costs the very conversion the
      // cache exists to skip.
      const uint64_t every_nth = ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache_verify_every_nth.Get();
      if (!every_nth || (hit_index % every_nth) != 0) {
        dxil_out = std::move(cached);
        return true;
      }
      std::vector<uint8_t> fresh;
      std::string verify_error;
      const bool converted = dxil_converter_raw_.Convert(dxbc, fresh, &verify_error);
      const MetalArtifactHitVerdict verdict =
          converted ? VerifyMetalArtifactHit(cached, fresh)
                    : MetalArtifactHitVerdict::kMissUnverifiable;
      census.dxil_verify.Record(verdict);
      if (MetalArtifactHitIsServable(verdict)) {
        dxil_out = std::move(cached);
        return true;
      }
      // A served artifact this build does not reproduce. Report it, drop the
      // entry so it rebuilds, and use the FRESH bytes — never the cached ones.
      REXLOG_ERROR(
          "MetalDxilStore: VERIFICATION FAILURE at site '{}' (verdict={}, "
          "cached={} B, fresh={} B) — the served artifact is not what this "
          "build produces. Entry dropped and rebuilt.",
          site, MetalArtifactHitVerdictName(verdict), cached.size(),
          fresh.size());
      {
        std::lock_guard<std::mutex> lock(durable_store_mutex_);
        dxil_store_.Forget(key);
      }
      if (!converted || fresh.empty()) {
        if (error_out) {
          *error_out = verify_error.empty()
                           ? std::string("verification re-convert failed")
                           : verify_error;
        }
        return false;
      }
      dxil_out = std::move(fresh);
      return true;
    }
  }

  // Cold conversion. Timed into the census at EVERY site, so the per-stage
  // totals finally cover geometry and tessellation too.
  //
  // The timer scope ends BEFORE the store write on purpose: census.dxil must
  // mean "dxilconv time" and nothing else, or enabling the cache appears to
  // make conversion slower. A first attempt left Store() inside the scope and
  // the cold arm's dxil total rose by ~30 ms with the gate on — cache
  // bookkeeping being reported as converter cost, which is exactly the
  // measurement error a cold-regression promotion gate would then act on.
  {
    MetalShaderBuildTimer timer(census.dxil);
    if (!dxil_converter_raw_.Convert(dxbc, dxil_out, error_out)) {
      timer.Dismiss();
      return false;
    }
    timer.set_bytes(dxil_out.size());
  }
  if (cache_enabled) {
    const auto store_start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(durable_store_mutex_);
      dxil_store_.Store(key, dxil_out);
    }
    census.dxil_store_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - store_start)
                     .count()),
        std::memory_order_relaxed);
  }
  return true;
}

bool MetalPipelineCache::TranslateShader(
    MetalShader::MetalTranslation& translation) {
  return TranslateShader(translation, *shader_translator_, metal_converter_);
}

bool MetalPipelineCache::TranslateShader(
    MetalShader::MetalTranslation& translation,
    ::rex::gpu::DXBCShaderTranslator& translator, MetalShaderConverter& metal_converter) {
  std::lock_guard<std::mutex> translate_lock(
      static_cast<MetalShader&>(translation.GetShader()).translate_mutex());
  // Published on every return, before the mutex is released: the outcome
  // below is final.
  struct MarkTranslated {
    MetalShader::MetalTranslation& translation;
    ~MarkTranslated() { translation.MarkMetalTranslated(); }
  } mark_translated{translation};
  if (translation.IsTranslated()) {
    return translation.IsValid() && translation.has_metal_function();
  }

  MetalShader& shader = static_cast<MetalShader&>(translation.GetShader());


  // Stage 1: microcode -> DXBC (reused ::rex::gpu::DXBCShaderTranslator).
  // Task 1.1 census: timed even though it is the cheap stage, because it is
  // the floor a metallib hit cannot go below (it runs on a hit too).
  MetalShaderBuildCensus& census = GetMetalShaderBuildCensus();
  {
    MetalShaderBuildTimer timer(census.dxbc);
    if (!translator.TranslateAnalyzedShader(translation)) {
      timer.Dismiss();
      REXLOG_ERROR("MetalPipelineCache: DXBC translation failed for shader {:016X}",
             shader.GetUcodeDataHash());
      return false;
    }
    timer.set_bytes(translation.GetTranslatedBinary().size());
  }
  const std::vector<uint8_t>& dxbc = translation.GetTranslatedBinary();
  if (dxbc.empty()) {
    REXLOG_ERROR("MetalPipelineCache: empty DXBC for shader {:016X}",
           shader.GetUcodeDataHash());
    return false;
  }

  // M1 (3.7/3.8) — durable metallib store. Stage 1 above always runs even on a
  // hit: it costs 0.8-0.9% of translation time (~0.06 ms mean, measured in
  // task 3.9) and it keeps translated_binary() populated, which the
  // geometry-emulation stages consume. Skipping it to save that would break
  // geometry emulation for a saving smaller than the measurement noise.
  //
  // What a hit DOES skip is stages 2 and 3 — DXBC->DXIL and DXIL->metallib —
  // which are 99.7-99.8% of translation cost.
  const uint64_t m1_key = MetallibCacheKey(translation);
  if (::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() && storage_.is_open()) {
    std::vector<uint8_t> cached_metallib;
    const auto load_start = std::chrono::steady_clock::now();
    const bool loaded = storage_.LoadMetallib(m1_key, &cached_metallib);
    census.metallib_load_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - load_start)
                     .count()),
        std::memory_order_relaxed);
    if (loaded) {
      MetalShaderBuildTimer timer(census.function);
      if (translation.CreateMetalFunction(device_, cached_metallib)) {
        timer.set_bytes(cached_metallib.size());
        census.metallib_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      timer.Dismiss();
      // A stored blob the device will not load is not trustworthy — fall
      // through to a full cold translation rather than failing the draw.
      // Counted as REJECTED, never as a miss: a miss is cold, this is a stored
      // artifact that is wrong, and one number covering both would hide the
      // second inside the first.
      census.metallib_rejected.fetch_add(1, std::memory_order_relaxed);
      REXLOG_WARN(
          "MetalPipelineCache: cached metallib {:016X} failed to load; "
          "retranslating",
          m1_key);
    } else {
      census.metallib_misses.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Stage 2: DXBC -> DXIL, through the ONE cache-aware helper every conversion
  // site uses (see ConvertDxbcToDxilCached).
  std::string error;
  std::vector<uint8_t> dxil;
  if (!ConvertDxbcToDxilCached(dxbc, dxil, &error, "vertex/pixel")) {
    REXLOG_ERROR("MetalPipelineCache: DXBC->DXIL failed for shader {:016X}: {}",
           shader.GetUcodeDataHash(), error);
    return false;
  }

  // Shader-chain forensics (XE_METAL_DUMP_XLAT=<hex ucode hash>): dump the
  // translated DXBC + DXIL + metallib for that shader to /tmp for offline
  // disassembly (the light-VS frc-unpack outputs zero; the M4 GPU primitive
  // is proven exact, so the divergence is inside this chain).
  static const uint64_t s_dump_xlat = [] {
    const char* v = std::getenv("XE_METAL_DUMP_XLAT");
    return v ? strtoull(v, nullptr, 16) : 0ull;
  }();
  if (s_dump_xlat && translation.GetShader().GetUcodeDataHash() == s_dump_xlat) {
    auto dump = [&](const char* ext, const std::vector<uint8_t>& data) {
      std::string path =
          fmt::format("/tmp/xe_xlat_{:016X}_{:016X}.{}", s_dump_xlat, translation.GetModification(), ext);
      FILE* f = fopen(path.c_str(), "wb");
      if (f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        REXLOG_WARN("MetalPipelineCache: dumped {} ({} bytes)", path, data.size());
      }
    };
    dump("dxbc", dxbc);
  }
  // Stage 3: DXIL -> metallib.
  MetalShaderConverter::Stage stage =
      shader.GetType() == xenos::ShaderType::kVertex
          ? MetalShaderConverter::Stage::kVertex
          : MetalShaderConverter::Stage::kFragment;
  if (s_dump_xlat && translation.GetShader().GetUcodeDataHash() == s_dump_xlat) {
    std::string path = fmt::format("/tmp/xe_xlat_{:016X}_{:016X}.dxil", s_dump_xlat, translation.GetModification());
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
      fwrite(dxil.data(), 1, dxil.size(), f);
      fclose(f);
      REXLOG_WARN("MetalPipelineCache: dumped {} ({} bytes)", path, dxil.size());
    }
  }
  std::vector<uint8_t> metallib;
  {
    MetalShaderBuildTimer timer(census.msc);
    MetalStageRequestProbe probe(dxil, uint32_t(stage),
                                 uint32_t(MetalShaderConverter::InputTopology::
                                              kUndefined));
    if (!metal_converter.Convert(stage, dxil, metallib, &error)) {
      timer.Dismiss();
      REXLOG_ERROR("MetalPipelineCache: DXIL->metallib failed for shader {:016X}: {}",
             shader.GetUcodeDataHash(), error);
      return false;
    }
    timer.set_bytes(metallib.size());
  }

  // Load onto the device as an MTLFunction.
  if (s_dump_xlat && translation.GetShader().GetUcodeDataHash() == s_dump_xlat) {
    std::string path =
        fmt::format("/tmp/xe_xlat_{:016X}_{:016X}.metallib", s_dump_xlat, translation.GetModification());
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
      fwrite(metallib.data(), 1, metallib.size(), f);
      fclose(f);
      REXLOG_WARN("MetalPipelineCache: dumped {} ({} bytes)", path, metallib.size());
    }
  }
  {
    MetalShaderBuildTimer timer(census.function);
    if (!translation.CreateMetalFunction(device_, metallib)) {
      timer.Dismiss();
      REXLOG_ERROR(
          "MetalPipelineCache: MTLFunction creation failed for shader {:016X}",
          shader.GetUcodeDataHash());
      return false;
    }
    timer.set_bytes(metallib.size());
  }

  // Publish only after the device has accepted it, so a blob that cannot load
  // is never written for a later run to trip over.
  if (::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() && storage_.is_open()) {
    const auto store_start = std::chrono::steady_clock::now();
    bool stored;
    {
      std::lock_guard<std::mutex> lock(durable_store_mutex_);
      stored = storage_.StoreMetallib(m1_key, metallib);
    }
    if (stored) {
      census.metallib_stores.fetch_add(1, std::memory_order_relaxed);
      census.metallib_store_bytes.fetch_add(metallib.size(),
                                            std::memory_order_relaxed);
    }
    census.metallib_store_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - store_start)
                     .count()),
        std::memory_order_relaxed);
  }
  return true;
}

namespace {

// 3.2 — content-derived shader identity for the pipeline key.
//
// The key previously used MTLFunction / translation POINTERS. Those are stable
// within one run and meaningless across runs, so no durable entry could be
// written against them. (ucode hash, modification, type) is the equivalent
// identity in content terms: MetalShader owns exactly one translation per
// modification and TranslateShader is idempotent per translation, so the
// pointer and this triple are 1:1 within a run — the swap cannot merge two
// distinct pipelines or split one.
//
// `present` keeps a null pixel shader distinct from any fragment-carrying
// pipeline, which the spec requires and which the old key got from a null
// pointer. A real shader can never collide with the absent case because
// present=0 zeroes the whole triple.
struct MetalShaderKeyIdentity {
  uint64_t ucode_hash;
  uint64_t modification;
  uint32_t shader_type;
  uint32_t present;
};

MetalShaderKeyIdentity MakeShaderKeyIdentity(
    const MetalShader::MetalTranslation* translation) {
  MetalShaderKeyIdentity identity = {};
  if (!translation) {
    return identity;
  }
  identity.ucode_hash = translation->GetShader().GetUcodeDataHash();
  identity.modification = translation->GetModification();
  identity.shader_type = uint32_t(translation->GetShader().GetType());
  identity.present = 1;
  return identity;
}

// The COMPLETE pipeline-creation identity for a geometry-emulation pipeline —
// the exact bytes hashed into its cache key. Everything MTLRenderPipelineState
// creation consumes is here: shader identities, every MRT format, blend
// controls, write mask, depth (and thereby stencil: Depth32Float_Stencil8
// implies the stencil attachment) format, and sample count.
//
// Depth/stencil ENABLE/WRITE state is deliberately absent: on Metal that lives
// in MTLDepthStencilState, set per-encoder, and does not participate in
// pipeline creation — logging it here would imply a discriminator that cannot
// exist at this boundary.
struct GeometryPipelineKeyBytes {
  uint32_t gs_key;
  uint32_t pad0;
  MetalShaderKeyIdentity vs;  // object-stage source translation.
  MetalShaderKeyIdentity ps;  // fragment stage.
  uint64_t colors[xenos::kMaxColorRenderTargets];
  uint64_t depth;
  uint32_t sample_count;
  uint32_t blends[xenos::kMaxColorRenderTargets];
  uint32_t write_mask;
  uint32_t pad;
};

// One formatter for BOTH the success and failure logs, so they cannot drift
// apart: the first attribution round found the same geometry triple both
// succeeding and failing, and the depth-format correlation could not be tested
// precisely because successes logged fewer fields than failures.
std::string DescribeGeometryPipelineKey(uint64_t key_digest,
                                        const GeometryPipelineKeyBytes& kb) {
  return fmt::format(
      "key={:016X} gs_key={:08X} vs={:016X}/{:08X} ps={:016X}/{:08X} "
      "colors=[{:X},{:X},{:X},{:X}] blends=[{:08X},{:08X},{:08X},{:08X}] "
      "write_mask={:X} depth_fmt={} samples={}",
      key_digest, kb.gs_key, kb.vs.ucode_hash, uint32_t(kb.vs.modification),
      kb.ps.ucode_hash, uint32_t(kb.ps.modification), kb.colors[0],
      kb.colors[1], kb.colors[2], kb.colors[3], kb.blends[0], kb.blends[1],
      kb.blends[2], kb.blends[3], kb.write_mask, kb.depth, kb.sample_count);
}

MTLBlendFactor ToMetalBlendFactor(MetalBlendFactor factor) {
  switch (factor) {
    case MetalBlendFactor::kZero:
      return MTLBlendFactorZero;
    case MetalBlendFactor::kOne:
      return MTLBlendFactorOne;
    case MetalBlendFactor::kSourceColor:
      return MTLBlendFactorSourceColor;
    case MetalBlendFactor::kOneMinusSourceColor:
      return MTLBlendFactorOneMinusSourceColor;
    case MetalBlendFactor::kSourceAlpha:
      return MTLBlendFactorSourceAlpha;
    case MetalBlendFactor::kOneMinusSourceAlpha:
      return MTLBlendFactorOneMinusSourceAlpha;
    case MetalBlendFactor::kDestinationColor:
      return MTLBlendFactorDestinationColor;
    case MetalBlendFactor::kOneMinusDestinationColor:
      return MTLBlendFactorOneMinusDestinationColor;
    case MetalBlendFactor::kDestinationAlpha:
      return MTLBlendFactorDestinationAlpha;
    case MetalBlendFactor::kOneMinusDestinationAlpha:
      return MTLBlendFactorOneMinusDestinationAlpha;
    case MetalBlendFactor::kBlendColor:
      return MTLBlendFactorBlendColor;
    case MetalBlendFactor::kOneMinusBlendColor:
      return MTLBlendFactorOneMinusBlendColor;
    case MetalBlendFactor::kBlendAlpha:
      return MTLBlendFactorBlendAlpha;
    case MetalBlendFactor::kOneMinusBlendAlpha:
      return MTLBlendFactorOneMinusBlendAlpha;
    case MetalBlendFactor::kSourceAlphaSaturated:
      return MTLBlendFactorSourceAlphaSaturated;
    default:
      return MTLBlendFactorZero;
  }
}

MTLBlendOperation ToMetalBlendOperation(MetalBlendOperation operation) {
  switch (operation) {
    case MetalBlendOperation::kAdd:
      return MTLBlendOperationAdd;
    case MetalBlendOperation::kSubtract:
      return MTLBlendOperationSubtract;
    case MetalBlendOperation::kMin:
      return MTLBlendOperationMin;
    case MetalBlendOperation::kMax:
      return MTLBlendOperationMax;
    case MetalBlendOperation::kReverseSubtract:
      return MTLBlendOperationReverseSubtract;
    default:
      return MTLBlendOperationAdd;
  }
}

// Applies one normalized RB_BLENDCONTROL + RB_COLOR_MASK nibble to a color-
// attachment descriptor. Shared by the standard render pipeline and the
// geometry-emulation mesh pipeline (both use
// MTLRenderPipelineColorAttachmentDescriptor).
void ApplyRtBlendState(MTLRenderPipelineColorAttachmentDescriptor* rt,
                       uint32_t blend_control, uint32_t color_write_mask) {
  MetalColorAttachmentBlendState state =
      GetMetalColorAttachmentBlendState(blend_control, color_write_mask);
  MTLColorWriteMask write_mask = MTLColorWriteMaskNone;
  if (state.write_mask & 0b0001) {
    write_mask |= MTLColorWriteMaskRed;
  }
  if (state.write_mask & 0b0010) {
    write_mask |= MTLColorWriteMaskGreen;
  }
  if (state.write_mask & 0b0100) {
    write_mask |= MTLColorWriteMaskBlue;
  }
  if (state.write_mask & 0b1000) {
    write_mask |= MTLColorWriteMaskAlpha;
  }
  rt.writeMask = write_mask;
  rt.blendingEnabled = state.blending_required;
  // XE_METAL_NO_BLEND=1: drop every blend to a plain write. The frame is uniformly lifted in green and blue, added
  // rather than scaled, which is what an over-strong blend does; with blending off, a blend fault shows as a plainly
  // different (usually darker, harder-edged) frame, while a fault elsewhere in the draw path survives unchanged.
  static const bool s_no_blend = std::getenv("XE_METAL_NO_BLEND") != nullptr;
  if (s_no_blend) {
    rt.blendingEnabled = NO;
    return;
  }
  if (!state.blending_required) {
    return;
  }
  rt.sourceRGBBlendFactor = ToMetalBlendFactor(state.rgb.source_factor);
  rt.destinationRGBBlendFactor = ToMetalBlendFactor(state.rgb.destination_factor);
  rt.rgbBlendOperation = ToMetalBlendOperation(state.rgb.operation);
  rt.sourceAlphaBlendFactor = ToMetalBlendFactor(state.alpha.source_factor);
  rt.destinationAlphaBlendFactor = ToMetalBlendFactor(state.alpha.destination_factor);
  rt.alphaBlendOperation = ToMetalBlendOperation(state.alpha.operation);
}

// Maps a geometry-shader type to the MSC input topology the guest primitive
// feeds the emulation with (rect/quad expand from triangles, points from points),
// mirroring XeniOS's GetGeometryStageInputTopology.
MetalShaderConverter::InputTopology GeometryInputTopology(
    PipelineGeometryShader type) {
  switch (type) {
    case PipelineGeometryShader::kPointList:
      return MetalShaderConverter::InputTopology::kPoint;
    case PipelineGeometryShader::kRectangleList:
      return MetalShaderConverter::InputTopology::kTriangle;
    case PipelineGeometryShader::kQuadList:
      // The generated DXBC GS declares line-with-adjacency. MSC's input
      // topology describes the base primitive family; the adjacency arity
      // comes from the shader declaration itself.
      return MetalShaderConverter::InputTopology::kLine;
    default:
      return MetalShaderConverter::InputTopology::kUndefined;
  }
}
}  // namespace

void* MetalPipelineCache::GetOrCreateMemexportPipelineState(
    MetalShader::MetalTranslation* vertex) {
  // Memexport-only draws (VS eA/eM exports): Metal requires a void-returning
  // vertex function for rasterizationEnabled=NO, but MSC-translated shaders
  // return a stage-out struct — so rasterization stays ON into a throwaway
  // scratch depth attachment (DontCare load/store; the command processor's
  // memexport pass provides it). That makes this exactly the existing
  // depth-only pipeline variant: no fragment function, no color attachment.
  uint64_t color_pixel_formats[xenos::kMaxColorRenderTargets] = {};
  uint32_t blend_controls[xenos::kMaxColorRenderTargets] = {};
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_pixel_formats[i] = uint64_t(MTLPixelFormatInvalid);
  }
  return GetOrCreateRenderPipelineState(
      vertex, /*pixel=*/nullptr, color_pixel_formats,
      MetalRenderTargetCache::kDepthFormat,
      MetalPipelineRasterKey{/*raster_sample_count=*/1},
      blend_controls, /*color_write_mask=*/0);
}

void* MetalPipelineCache::GetOrCreateRenderPipelineState(
    MetalShader::MetalTranslation* vertex,
    MetalShader::MetalTranslation* pixel, const uint64_t* color_pixel_formats,
    uint64_t depth_pixel_format, MetalPipelineRasterKey raster_key,
    const uint32_t* blend_controls,
    uint32_t color_write_mask) {
  if (!vertex || !vertex->has_metal_function()) {
    return nullptr;
  }
  const uint64_t key =
      RenderPipelineKey(vertex, pixel, color_pixel_formats, depth_pixel_format,
                        raster_key, blend_controls, color_write_mask);
  auto it = render_pipeline_states_.find(key);
  if (it != render_pipeline_states_.end()) {
    return it->second;
  }
  MTLRenderPipelineDescriptor* desc =
      (__bridge_transfer MTLRenderPipelineDescriptor*)
          NewRenderPipelineDescriptor(vertex, pixel, color_pixel_formats,
                                      depth_pixel_format, raster_key,
                                      blend_controls, color_write_mask,
                                      binary_archive_lookup_list_);
  bool archive_stale = false;
  void* state =
      NewRenderPipelineState((__bridge void*)desc, key, &archive_stale);
  if (archive_stale) {
    archived_pipeline_keys_.erase(key);
  }
  void* state_ptr = AddRenderPipelineState(key, (__bridge void*)desc, state);
  if (state_ptr) {
    StorePipeline(vertex,
                  pixel && pixel->has_metal_function() ? pixel : nullptr,
                  color_pixel_formats, depth_pixel_format,
                  raster_key.raster_sample_count, blend_controls,
                  color_write_mask);
  }
  return state_ptr;
}

uint64_t MetalPipelineCache::RenderPipelineKey(
    MetalShader::MetalTranslation* vertex,
    MetalShader::MetalTranslation* pixel, const uint64_t* color_pixel_formats,
    uint64_t depth_pixel_format, MetalPipelineRasterKey raster_key,
    const uint32_t* blend_controls, uint32_t color_write_mask) {
  // 3.2 — full pipeline identity, content-derived so it can back a durable
  // entry. Audit of every input that reaches the descriptor below:
  //   shader identities + modifications  -> vs / ps (MakeShaderKeyIdentity)
  //   null-vs-present pixel shader       -> ps.present
  //   render-target formats              -> colors
  //   depth format                       -> depth (the stencil format is
  //                                         derived from it, never independent)
  //   blend state                        -> blends
  //   write mask                         -> write_mask
  //   sample state                       -> sample_count
  //   vertex layout                      -> NOT APPLICABLE: this backend never
  //                                         sets a vertexDescriptor; MSC-
  //                                         translated shaders fetch vertices
  //                                         themselves, so no layout reaches
  //                                         the pipeline
  //   alphaToCoverageEnabled             -> constant NO (Xenos alpha-to-mask is
  //                                         exact SV_Coverage in the shader),
  //                                         so it is not an identity input
  struct KeyBytes {
    MetalShaderKeyIdentity vs;
    MetalShaderKeyIdentity ps;
    uint64_t colors[xenos::kMaxColorRenderTargets];
    uint64_t depth;
    uint32_t sample_count;
    uint32_t blends[xenos::kMaxColorRenderTargets];
    uint32_t write_mask;
    uint32_t pad;
  } key_bytes = {};
  key_bytes.vs = MakeShaderKeyIdentity(vertex);
  // A pixel translation without a function produces the same depth-only
  // pipeline as no pixel translation at all (see the descriptor below), so it
  // must key the same. The old pointer key got this for free because
  // metal_function() was null in both cases.
  key_bytes.ps = MakeShaderKeyIdentity(
      pixel && pixel->has_metal_function() ? pixel : nullptr);
  key_bytes.depth = depth_pixel_format;
  key_bytes.sample_count = raster_key.raster_sample_count;
  key_bytes.write_mask = color_write_mask;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    key_bytes.colors[i] = color_pixel_formats[i];
    key_bytes.blends[i] = blend_controls[i];
  }
  return XXH3_64bits(&key_bytes, sizeof(key_bytes));
}

void* MetalPipelineCache::NewRenderPipelineDescriptor(
    MetalShader::MetalTranslation* vertex,
    MetalShader::MetalTranslation* pixel, const uint64_t* color_pixel_formats,
    uint64_t depth_pixel_format, MetalPipelineRasterKey raster_key,
    const uint32_t* blend_controls, uint32_t color_write_mask,
    void* lookup_list) const {
  const MetalShaderKeyIdentity vs = MakeShaderKeyIdentity(vertex);
  const MetalShaderKeyIdentity ps = MakeShaderKeyIdentity(
      pixel && pixel->has_metal_function() ? pixel : nullptr);
  MTLRenderPipelineDescriptor* desc =
      [[MTLRenderPipelineDescriptor alloc] init];
  desc.rasterSampleCount = raster_key.raster_sample_count;
  // Xenos alpha-to-mask is emitted as exact dynamic SV_Coverage by the reused
  // DXBC translator. Enabling Metal alpha-to-coverage here would apply a
  // second, host-defined coverage filter and would incorrectly require alpha
  // state in the pipeline key.
  desc.alphaToCoverageEnabled = NO;
  // Guest shader identities in the pipeline label: Instruments' Shader
  // Timeline attributes GPU samples to the pipeline label (function names are
  // DXIL hashes, not guest ucode hashes), which is the only per-shader GPU-time
  // attribution on Apple GPUs (no per-draw counter sampling).
  desc.label = [NSString
      stringWithFormat:@"xe vs=%016llX.%llX ps=%016llX.%llX s%u",
                       (unsigned long long)vs.ucode_hash,
                       (unsigned long long)vs.modification,
                       (unsigned long long)ps.ucode_hash,
                       (unsigned long long)ps.modification,
                       raster_key.raster_sample_count];
  desc.vertexFunction = (__bridge id<MTLFunction>)vertex->metal_function();
  if (pixel && pixel->has_metal_function()) {
    desc.fragmentFunction = (__bridge id<MTLFunction>)pixel->metal_function();
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (color_pixel_formats[i] == MTLPixelFormatInvalid) {
      continue;
    }
    desc.colorAttachments[i].pixelFormat =
        (MTLPixelFormat)color_pixel_formats[i];
    // Per-target blending and write mask from RB_BLENDCONTROL[0-3] and the
    // corresponding RB_COLOR_MASK nibble.
    ApplyRtBlendState(desc.colorAttachments[i], blend_controls[i],
                      (color_write_mask >> (i * 4u)) & 0xFu);
  }
  {
    const MTLPixelFormat depth_format =
        (MTLPixelFormat)(depth_pixel_format & ~kDepthWithoutStencilBit);
    if (depth_format != MTLPixelFormatInvalid) {
      desc.depthAttachmentPixelFormat = depth_format;
      if (depth_format == MTLPixelFormatDepth32Float_Stencil8 &&
          !(depth_pixel_format & kDepthWithoutStencilBit)) {
        desc.stencilAttachmentPixelFormat = depth_format;
      }
    }
  }
  if (lookup_list) {
    desc.binaryArchives = (__bridge NSArray*)lookup_list;
  }
  // XE_METAL_VALIDATION_OFF (diagnostic, with MTL_SHADER_VALIDATION=1):
  // leaves guest draw pipelines uninstrumented so a GPU hang that shader
  // validation hides can be bisected to pipelines. "draws" = every draw
  // pipeline; "rect"/"norect" = those whose vertex shader does / does not
  // expand rectangle lists; "bit:K:V" = those whose bit K of (vs ^ ps ucode
  // hash) is V;
  // "vs:H1,H2" = those whose vertex shader ucode hash is in the list.
  static const std::string s_validation_off = [] {
    const char* v = std::getenv("XE_METAL_VALIDATION_OFF");
    if (!v && std::getenv("XE_METAL_VALIDATION_OFF_OTHER")) {
      v = "none";
    }
    return std::string(v ? v : "");
  }();
  if (!s_validation_off.empty()) {
    if (@available(macOS 15.0, *)) {
      const uint64_t mixed = vs.ucode_hash ^ ps.ucode_hash;
      bool off = false;
      if (s_validation_off == "draws") {
        off = true;
      } else if (s_validation_off == "rect" || s_validation_off == "norect") {
        const bool rect =
            vertex && ::rex::gpu::DXBCShaderTranslator::Modification(
                          vertex->GetModification())
                              .vertex.host_vertex_shader_type ==
                          ::rex::gpu::Shader::HostVertexShaderType::
                              kRectangleListAsTriangleStrip;
        off = rect == (s_validation_off == "rect");
      } else if (s_validation_off.rfind("bit:", 0) == 0) {
        unsigned bit = 0, value = 0;
        if (std::sscanf(s_validation_off.c_str() + 4, "%u:%u", &bit,
                        &value) == 2 &&
            bit < 64) {
          off = ((mixed >> bit) & 1) == value;
        }
      } else if (s_validation_off.rfind("vs:", 0) == 0) {
        off = s_validation_off.find(fmt::format("{:016X}", vs.ucode_hash)) !=
              std::string::npos;
      }
      // Explicit either way, so XE_METAL_VALIDATION_OFF_OTHER's hook (which
      // only changes pipelines left at the default) leaves these alone.
      desc.shaderValidation =
          off ? MTLShaderValidationDisabled : MTLShaderValidationEnabled;
      static std::atomic<uint32_t> s_logged{0};
      if (s_logged.fetch_add(1, std::memory_order_relaxed) < 4096) {
        REXLOG_INFO("MetalValidationOff: vs={:016X} ps={:016X} validation={}",
               vs.ucode_hash, ps.ucode_hash, off ? "off" : "on");
      }
    }
  }
  return (__bridge_retained void*)desc;
}

void* MetalPipelineCache::NewRenderPipelineState(
    void* descriptor, uint64_t key, bool* archive_stale_out) const {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLRenderPipelineDescriptor* desc =
      (__bridge MTLRenderPipelineDescriptor*)descriptor;
  NSError* error = nil;
  id<MTLRenderPipelineState> state = nil;
  // M2 — a key already archived is looked up fail-fast, so a hit is provably
  // served from the archive (not from the macOS shader cache, the confound
  // that made the first M2 study measure nothing) and a stale entry costs one
  // lookup before the cold build below.
  if (binary_archive_lookup_list_ && archived_pipeline_keys_.count(key)) {
    MetalShaderBuildTimer timer(GetMetalShaderBuildCensus().pso);
    state = [device
        newRenderPipelineStateWithDescriptor:desc
                                     options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                  reflection:nil
                                       error:nil];
    if (state) {
      GetMetalShaderBuildCensus().archive_hits.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      timer.Dismiss();
      GetMetalShaderBuildCensus().archive_stale.fetch_add(
          1, std::memory_order_relaxed);
      *archive_stale_out = true;
    }
  }
  if (!state) {
    // Task 1.1 census: host pipeline-state creation, the cost M2 (the binary
    // archive) targets and the one the async stage would move off the frame
    // path. Timed around the API call alone; descriptor assembly is ours and
    // is not what either mechanism removes.
    MetalShaderBuildTimer timer(GetMetalShaderBuildCensus().pso);
    state = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!state && desc.binaryArchives.count) {
      // Under MTL_SHADER_VALIDATION even the plain call fails on an archive
      // miss ("Unable to find function ... in binary archives"), so every
      // draw was skipped: build once more with no archives attached.
      MTLRenderPipelineDescriptor* plain = [desc copy];
      plain.binaryArchives = nil;
      error = nil;
      state = [device newRenderPipelineStateWithDescriptor:plain error:&error];
    }
    if (!state) {
      timer.Dismiss();
      REXLOG_ERROR("MetalPipelineCache: newRenderPipelineState failed: {}",
             error ? [[error localizedDescription] UTF8String] : "unknown");
      return nullptr;
    }
  }
  void* state_ptr = (__bridge_retained void*)state;
  // XE_METAL_PIPELINE_LOG=1: every render pipeline built, for matching the
  // pso= pointers a fault report's rect draws name.
  static const bool s_pipeline_log = [] {
    const char* v = std::getenv("XE_METAL_PIPELINE_LOG");
    return v && v[0] == '1';
  }();
  if (s_pipeline_log) {
    REXLOG_INFO("MetalPipelineNew: key={:016X} pso={} vs={} from={}", key,
           state_ptr,
           desc.vertexFunction ? [desc.vertexFunction.name UTF8String] : "-",
           (archive_stale_out && !*archive_stale_out &&
            binary_archive_lookup_list_ && archived_pipeline_keys_.count(key))
               ? "archive"
               : "compiled");
  }
  return state_ptr;
}

void* MetalPipelineCache::AddRenderPipelineState(uint64_t key,
                                                 void* descriptor,
                                                 void* state_retained) {
  id<MTLRenderPipelineState> state =
      (__bridge_transfer id<MTLRenderPipelineState>)state_retained;
  MTLRenderPipelineDescriptor* desc =
      (__bridge MTLRenderPipelineDescriptor*)descriptor;
  id<MTLBinaryArchive> archive = (__bridge id<MTLBinaryArchive>)binary_archive_;
  // While compacting, archive hits are added again too (once per session), so
  // the old segments they came from can be deleted at quit.
  const bool compacting = !binary_archive_compacting_.empty();
  if (state && archive && !storage_.admission_stopped() &&
      (compacting ? !binary_archive_session_keys_.count(key)
                  : !archived_pipeline_keys_.count(key))) {
    MetalShaderBuildTimer timer(GetMetalShaderBuildCensus().archive_add);
    NSError* add_error = nil;
    if ([archive addRenderPipelineFunctionsWithDescriptor:desc
                                                     error:&add_error]) {
      archived_pipeline_keys_.insert(key);
      binary_archive_pending_keys_.push_back(key);
      if (compacting) {
        binary_archive_session_keys_.insert(key);
      }
    } else {
      timer.Dismiss();
      REXLOG_WARN("MetalPipelineCache: binary archive add failed: {}",
             add_error ? [[add_error localizedDescription] UTF8String]
                       : "unknown");
    }
  }
  if (!state) {
    render_pipeline_states_.emplace(key, nullptr);
    return nullptr;
  }
  void* state_ptr = (__bridge_retained void*)state;
  render_pipeline_states_.emplace(key, state_ptr);
  return state_ptr;
}

bool MetalPipelineCache::AsyncPipelinesEnabled() {
  // XE_METAL_ASYNC_PIPELINES=0/1 overrides the cvar.
  static const int s_env = [] {
    const char* v = std::getenv("XE_METAL_ASYNC_PIPELINES");
    return v && (v[0] == '0' || v[0] == '1') && !v[1] ? v[0] - '0' : -1;
  }();
  return s_env >= 0 ? s_env == 1 : ::rex::graphics::metal::GetMetalSettings().metal_async_pipelines.Get();
}

uint64_t MetalPipelineCache::AsyncPipelineKey(
    MetalShader::MetalTranslation* vertex,
    MetalShader::MetalTranslation* pixel, const uint64_t* color_pixel_formats,
    uint64_t depth_pixel_format, MetalPipelineRasterKey raster_key,
    const uint32_t* blend_controls, uint32_t color_write_mask) {
  struct KeyBytes {
    MetalShaderKeyIdentity vs;
    MetalShaderKeyIdentity ps;
    uint64_t colors[xenos::kMaxColorRenderTargets];
    uint64_t depth;
    uint32_t sample_count;
    uint32_t blends[xenos::kMaxColorRenderTargets];
    uint32_t write_mask;
    uint32_t pad;
  } key_bytes = {};
  // The pixel translation's identity whether or not it has translated yet:
  // RenderPipelineKey folds a pixel shader without a function into the
  // depth-only key, which is only known afterwards.
  key_bytes.vs = MakeShaderKeyIdentity(vertex);
  key_bytes.ps = MakeShaderKeyIdentity(pixel);
  key_bytes.depth = depth_pixel_format;
  key_bytes.sample_count = raster_key.raster_sample_count;
  key_bytes.write_mask = color_write_mask;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    key_bytes.colors[i] = color_pixel_formats[i];
    key_bytes.blends[i] = blend_controls[i];
  }
  return XXH3_64bits(&key_bytes, sizeof(key_bytes));
}

MetalPipelineCache::AsyncPipelineStatus
MetalPipelineCache::GetRenderPipelineStateAsync(
    MetalShader::MetalTranslation* vertex,
    MetalShader::MetalTranslation* pixel, const uint64_t* color_pixel_formats,
    uint64_t depth_pixel_format, MetalPipelineRasterKey raster_key,
    const uint32_t* blend_controls, uint32_t color_write_mask,
    bool stand_in_allowed, bool may_defer, void** pipeline_out,
    MetalShader::MetalTranslation** pixel_out,
    std::shared_ptr<void>* deferred_out) {
  *pipeline_out = nullptr;
  *pixel_out = nullptr;
  deferred_out->reset();
  DrainAsyncPipelines();
  // Default on Intel/AMD only until it is measured on Apple GPUs;
  // XE_METAL_PIPELINE_MEMO=0/1 forces it either way.
  static const bool s_memo_enabled = [this] {
    const char* v = std::getenv("XE_METAL_PIPELINE_MEMO");
    if (v && v[0]) {
      return v[0] != '0';
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    return !(device && [device supportsFamily:MTLGPUFamilyApple7]);
  }();
  AsyncPipelineMemo& memo = async_memo_;
  const uint32_t sample_count = raster_key.raster_sample_count;
  if (s_memo_enabled && memo.pipeline && memo.vertex == vertex &&
      memo.pixel == pixel && memo.depth == depth_pixel_format &&
      memo.sample_count == sample_count &&
      memo.write_mask == color_write_mask &&
      !std::memcmp(memo.colors, color_pixel_formats, sizeof(memo.colors)) &&
      !std::memcmp(memo.blends, blend_controls, sizeof(memo.blends))) {
    *pipeline_out = memo.pipeline;
    *pixel_out = memo.pixel_used;
    return AsyncPipelineStatus::kReady;
  }
  auto from_key = [&](uint64_t key, MetalShader::MetalTranslation* pixel_used) {
    auto it = render_pipeline_states_.find(key);
    if (it == render_pipeline_states_.end() || !it->second) {
      return AsyncPipelineStatus::kFailed;
    }
    *pipeline_out = it->second;
    *pixel_out = pixel_used;
    return AsyncPipelineStatus::kReady;
  };
  auto remember = [&]() {
    memo.vertex = vertex;
    memo.pixel = pixel;
    std::memcpy(memo.colors, color_pixel_formats, sizeof(memo.colors));
    memo.depth = depth_pixel_format;
    memo.sample_count = sample_count;
    std::memcpy(memo.blends, blend_controls, sizeof(memo.blends));
    memo.write_mask = color_write_mask;
    memo.pipeline = *pipeline_out;
    memo.pixel_used = *pixel_out;
  };
  const uint64_t async_key =
      AsyncPipelineKey(vertex, pixel, color_pixel_formats, depth_pixel_format,
                       raster_key, blend_controls, color_write_mask);
  {
    auto it = async_results_.find(async_key);
    if (it != async_results_.end()) {
      if (!it->second.key) {
        return AsyncPipelineStatus::kFailed;
      }
      AsyncPipelineStatus status =
          from_key(it->second.key, it->second.pixel_used);
      if (s_memo_enabled && status == AsyncPipelineStatus::kReady) {
        remember();
      }
      return status;
    }
  }
  // A state found or built below (stored pipelines replayed at startup,
  // synchronous builds) is recorded like a finished async build, so later
  // draws take the lookup above instead of hashing the key again and probing
  // three maps (GTA IV on the Mac Pro: ~0.3 ms/frame of the CP thread).
  auto ready = [&](uint64_t key, MetalShader::MetalTranslation* pixel_used) {
    AsyncPipelineStatus status = from_key(key, pixel_used);
    if (s_memo_enabled && status == AsyncPipelineStatus::kReady) {
      async_results_.emplace(async_key, AsyncPipelineResult{key, pixel_used});
      remember();
    }
    return status;
  };
  auto defer = [&](const std::shared_ptr<AsyncPipelineJob>& job) {
    *pixel_out = job->pixel_used;
    *deferred_out = job;
    GetMetalShaderBuildCensus().deferred_pipelines.fetch_add(
        1, std::memory_order_relaxed);
    return AsyncPipelineStatus::kDeferred;
  };
  auto new_job = [&]() {
    auto job = std::make_shared<AsyncPipelineJob>();
    job->vertex = vertex;
    job->pixel = pixel;
    std::memcpy(job->color_formats, color_pixel_formats,
                sizeof(job->color_formats));
    job->depth_format = depth_pixel_format;
    job->raster_key = raster_key;
    std::memcpy(job->blend_controls, blend_controls,
                sizeof(job->blend_controls));
    job->color_write_mask = color_write_mask;
    job->async_key = async_key;
    // The lookup list is replaced on the GPU thread when the archive is
    // saved; the worker keeps this one.
    job->lookup_list =
        binary_archive_lookup_list_
            ? (__bridge_retained void*)(__bridge NSArray*)
                  binary_archive_lookup_list_
            : nullptr;
    return job;
  };
  auto pending = async_pending_.find(async_key);
  if (pending != async_pending_.end()) {
    if (stand_in_allowed) {
      return AsyncPipelineStatus::kPending;
    }
    if (may_defer && pending->second->deferred) {
      return defer(pending->second);
    }
    // A draw that may not use a stand-in waits for the queued build.
    std::shared_ptr<AsyncPipelineJob> job = pending->second;
    {
      std::unique_lock<std::mutex> lock(async_mutex_);
      async_done_cv_.wait(lock, [&]() { return job->done; });
    }
    DrainAsyncPipelines();
    auto it = async_results_.find(async_key);
    return it != async_results_.end() && it->second.key
               ? from_key(it->second.key, it->second.pixel_used)
               : AsyncPipelineStatus::kFailed;
  }
  // Finished translations only. is_translated() is already true while a
  // background worker is still converting the same translation's DXIL and
  // metallib: read here it failed the draw as vs-translate (no error logged,
  // a different shader each run) or built the pipeline without the pixel
  // shader. XE_METAL_ASYNC_TRANSLATED_GATE=0 restores that check.
  static const bool s_finished_gate = [] {
    const char* v = std::getenv("XE_METAL_ASYNC_TRANSLATED_GATE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  auto finished = [](const MetalShader::MetalTranslation* translation) {
    return s_finished_gate ? translation->is_metal_translated()
                           : translation->IsTranslated();
  };
  const bool translated = finished(vertex) && (!pixel || finished(pixel));
  if (!translated || stand_in_allowed) {
    if (translated) {
      MetalShader::MetalTranslation* pixel_used =
          pixel && pixel->has_metal_function() ? pixel : nullptr;
      if (!vertex->has_metal_function()) {
        if (!vertex->is_metal_translated()) {
          // Only reachable with the gate off: the race above.
          static std::atomic<uint32_t> s_race_logs{0};
          if (s_race_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
            REXLOG_WARN(
                "MetalPipelineCache: vertex shader {:016X} still translating "
                "on a background worker; draw fails as vs-translate",
                vertex->GetShader().GetUcodeDataHash());
          }
        }
        return AsyncPipelineStatus::kFailed;
      }
      const uint64_t key =
          RenderPipelineKey(vertex, pixel_used, color_pixel_formats,
                            depth_pixel_format, raster_key, blend_controls,
                            color_write_mask);
      if (render_pipeline_states_.count(key)) {
        return ready(key, pixel_used);
      }
    }
    if (stand_in_allowed) {
      std::shared_ptr<AsyncPipelineJob> job = new_job();
      StartAsyncPipelineWorkers();
      async_pending_.emplace(async_key, job);
      {
        std::lock_guard<std::mutex> lock(async_mutex_);
        async_queue_.push_back(job);
      }
      async_queue_cv_.notify_one();
      return AsyncPipelineStatus::kPending;
    }
  }
  // Synchronous, as GetOrCreateRenderPipelineState's callers did, or with
  // only the translation synchronous when the state may be deferred.
  if (!TranslateShader(*vertex)) {
    return AsyncPipelineStatus::kFailed;
  }
  MetalShader::MetalTranslation* pixel_used =
      pixel && TranslateShader(*pixel) ? pixel : nullptr;
  if (may_defer) {
    const uint64_t key =
        RenderPipelineKey(vertex, pixel_used, color_pixel_formats,
                          depth_pixel_format, raster_key, blend_controls,
                          color_write_mask);
    if (render_pipeline_states_.count(key)) {
      return ready(key, pixel_used);
    }
    std::shared_ptr<AsyncPipelineJob> job = new_job();
    job->deferred = true;
    job->cache = this;
    job->pixel_used = pixel_used;
    StartAsyncPipelineWorkers();
    async_pending_.emplace(async_key, job);
    {
      // Ahead of stand-in builds: the encoder thread will wait for it.
      std::lock_guard<std::mutex> lock(async_mutex_);
      async_queue_.push_front(job);
    }
    async_queue_cv_.notify_one();
    return defer(job);
  }
  void* state = GetOrCreateRenderPipelineState(
      vertex, pixel_used, color_pixel_formats, depth_pixel_format, raster_key,
      blend_controls, color_write_mask);
  if (!state) {
    return AsyncPipelineStatus::kFailed;
  }
  *pipeline_out = state;
  *pixel_out = pixel_used;
  if (s_memo_enabled) {
    const uint64_t key =
        RenderPipelineKey(vertex, pixel_used, color_pixel_formats,
                          depth_pixel_format, raster_key, blend_controls,
                          color_write_mask);
    auto it = render_pipeline_states_.find(key);
    if (it != render_pipeline_states_.end() && it->second == state) {
      async_results_.emplace(async_key, AsyncPipelineResult{key, pixel_used});
      remember();
    }
  }
  return AsyncPipelineStatus::kReady;
}

MetalPipelineCache::AsyncPipelineJob::~AsyncPipelineJob() {
  if (deferred_state) {
    CFRelease(deferred_state);
  }
}

void* MetalPipelineCache::ResolveDeferredPipeline(void* request) {
  auto* job = static_cast<AsyncPipelineJob*>(request);
  MetalPipelineCache& cache = *job->cache;
  void* state;
  {
    std::unique_lock<std::mutex> lock(cache.async_mutex_);
    if (!job->done && !cache.async_stop_) {
      const auto start = std::chrono::steady_clock::now();
      cache.async_done_cv_.wait(
          lock, [&]() { return job->done || cache.async_stop_; });
      GetMetalShaderBuildCensus().RecordDeferredWait(uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start)
              .count()));
    }
    state = job->deferred_state;
  }
  if (state) {
    CFRetain(state);
  }
  return state;
}

void MetalPipelineCache::DrainAsyncPipelines() {
  // Uncontended, the lock alone was 0.5 ms of the Mac Pro's GTA IV command
  // processor frame (5,000 draws).
  if (!async_done_pending_.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::shared_ptr<AsyncPipelineJob>> done;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    async_done_pending_.store(false, std::memory_order_relaxed);
    if (async_done_.empty()) {
      return;
    }
    done.swap(async_done_);
  }
  for (const std::shared_ptr<AsyncPipelineJob>& job : done) {
    MTLRenderPipelineDescriptor* desc =
        (__bridge_transfer MTLRenderPipelineDescriptor*)job->descriptor;
    job->descriptor = nullptr;
    if (job->lookup_list) {
      NSArray* list = (__bridge_transfer NSArray*)job->lookup_list;
      (void)list;
      job->lookup_list = nullptr;
    }
    AsyncPipelineResult result = {0, nullptr};
    if (job->vertex_ok && desc) {
      if (render_pipeline_states_.count(job->key)) {
        // Built synchronously meanwhile.
        if (job->state) {
          id<MTLRenderPipelineState> state =
              (__bridge_transfer id<MTLRenderPipelineState>)job->state;
          (void)state;
        }
      } else if (AddRenderPipelineState(job->key, (__bridge void*)desc,
                                        job->state)) {
        StorePipeline(job->vertex, job->pixel_used, job->color_formats,
                      job->depth_format, job->raster_key.raster_sample_count,
                      job->blend_controls, job->color_write_mask);
      }
      job->state = nullptr;
      result = {job->key, job->pixel_used};
    }
    async_results_[job->async_key] = result;
    async_pending_.erase(job->async_key);
  }
}

void MetalPipelineCache::StartAsyncPipelineWorkers() {
  if (!async_workers_.empty()) {
    return;
  }
  // XE_METAL_ASYNC_PIPELINE_THREADS=<n>. Default a third of the logical
  // processors (at least 2): the guest keeps running, and a pipeline-state
  // build mostly waits on the Metal compiler service.
  static const uint32_t s_count = [] {
    if (const char* v = std::getenv("XE_METAL_ASYNC_PIPELINE_THREADS")) {
      const long n = std::strtol(v, nullptr, 10);
      if (n > 0) {
        return uint32_t(n);
      }
    }
    return std::max(rex::thread::logical_processor_count() / 3,
                    uint32_t(2));
  }();
  async_stop_ = false;
  for (uint32_t i = 0; i < s_count; ++i) {
    rex::thread::Thread::CreationParameters params;
    auto thread =
        rex::thread::Thread::Create(params, [this]() { AsyncPipelineWorker(); });
    if (!thread) {
      break;
    }
    thread->SetName("Metal Async Pipelines");
    async_workers_.push_back(std::move(thread));
  }
  REXLOG_INFO("MetalPipelineCache: {} background pipeline threads",
         async_workers_.size());
}

void MetalPipelineCache::StopAsyncPipelineWorkers() {
  if (async_workers_.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    async_stop_ = true;
  }
  async_queue_cv_.notify_all();
  // A deferred request still queued resolves to null (draws skipped).
  async_done_cv_.notify_all();
  for (auto& thread : async_workers_) {
    rex::thread::Wait(thread.get(), false);
  }
  async_workers_.clear();
  // Jobs never started: nothing to release but their lookup lists.
  for (const std::shared_ptr<AsyncPipelineJob>& job : async_queue_) {
    if (job->lookup_list) {
      NSArray* list = (__bridge_transfer NSArray*)job->lookup_list;
      (void)list;
      job->lookup_list = nullptr;
    }
  }
  async_queue_.clear();
  DrainAsyncPipelines();
  async_pending_.clear();
}

void MetalPipelineCache::AsyncPipelineWorker() {
  MetalShaderBuildCensus::AsyncBuildThread() = true;
  std::unique_ptr<::rex::gpu::DXBCShaderTranslator> translator = CreateShaderTranslator();
  MetalShaderConverter metal_converter;
  const bool converter_ok = metal_converter.Initialize(
      MetalTranslatorConfig::kMscBindlessResourcesUsed);
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  while (true) {
    std::shared_ptr<AsyncPipelineJob> job;
    {
      std::unique_lock<std::mutex> lock(async_mutex_);
      async_queue_cv_.wait(lock,
                           [&]() { return async_stop_ || !async_queue_.empty(); });
      if (async_stop_) {
        return;
      }
      job = async_queue_.front();
      async_queue_.pop_front();
    }
    @autoreleasepool {
      job->vertex_ok =
          converter_ok &&
          TranslateShader(*job->vertex, *translator, metal_converter);
      if (job->vertex_ok && !job->deferred) {
        // A deferred request's pixel shader is translated already, and the
        // command processor reads pixel_used.
        job->pixel_used =
            job->pixel &&
                    TranslateShader(*job->pixel, *translator, metal_converter)
                ? job->pixel
                : nullptr;
      }
      if (job->vertex_ok) {
        job->key = RenderPipelineKey(
            job->vertex, job->pixel_used, job->color_formats,
            job->depth_format, job->raster_key, job->blend_controls,
            job->color_write_mask);
        job->descriptor = NewRenderPipelineDescriptor(
            job->vertex, job->pixel_used, job->color_formats,
            job->depth_format, job->raster_key, job->blend_controls,
            job->color_write_mask, job->lookup_list);
        MTLRenderPipelineDescriptor* desc =
            (__bridge MTLRenderPipelineDescriptor*)job->descriptor;
        MetalShaderBuildTimer timer(GetMetalShaderBuildCensus().pso);
        NSError* error = nil;
        id<MTLRenderPipelineState> state =
            [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (state) {
          job->state = (__bridge_retained void*)state;
          if (job->deferred) {
            job->deferred_state = (__bridge_retained void*)state;
          }
        } else {
          timer.Dismiss();
          REXLOG_ERROR("MetalPipelineCache: background newRenderPipelineState "
                 "failed: {}",
                 error ? [[error localizedDescription] UTF8String] : "unknown");
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      job->done = true;
      async_done_.push_back(job);
      async_done_pending_.store(true, std::memory_order_release);
    }
    async_done_cv_.notify_all();
  }
}

void MetalPipelineCache::StorePipeline(MetalShader::MetalTranslation* vertex,
                                       MetalShader::MetalTranslation* pixel,
                                       const uint64_t* color_pixel_formats,
                                       uint64_t depth_pixel_format,
                                       uint32_t sample_count,
                                       const uint32_t* blend_controls,
                                       uint32_t color_write_mask) {
  if (shader_storage_replaying_ || !shader_storage_.is_active()) {
    return;
  }
  PipelineStoredDescription stored;
  std::memset(&stored, 0, sizeof(stored));
  StoredPipelineDescription& description = stored.description;
  auto& vertex_shader = static_cast<MetalShader&>(vertex->GetShader());
  description.vertex_shader_hash = vertex_shader.GetUcodeDataHash();
  description.vertex_shader_modification = vertex->GetModification();
  MetalShader* pixel_shader = nullptr;
  if (pixel) {
    pixel_shader = &static_cast<MetalShader&>(pixel->GetShader());
    ::rex::gpu::DXBCShaderTranslator::Modification modification(pixel->GetModification());
    if (const MetalShader::FetchPattern* pattern =
            pixel_shader->GetFetchPattern(
                modification.pixel.fetch_plain_pattern)) {
      std::memcpy(&description.pixel_fetch_pattern, pattern,
                  sizeof(description.pixel_fetch_pattern));
    }
    modification.pixel.fetch_plain_pattern = 0;
    description.pixel_shader_hash = pixel_shader->GetUcodeDataHash();
    description.pixel_shader_modification = modification.value;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    description.color_formats[i] = color_pixel_formats[i];
    description.blend_controls[i] = blend_controls[i];
  }
  description.depth_format = depth_pixel_format;
  description.sample_count = sample_count;
  description.color_write_mask = color_write_mask;
  stored.description_hash = XXH3_64bits(&description, sizeof(description));
  const uint32_t storage_index = shader_storage_.storage_index();
  for (MetalShader* shader : {&vertex_shader, pixel_shader}) {
    if (shader && shader->ucode_storage_index() != storage_index) {
      shader->set_ucode_storage_index(storage_index);
      shader_storage_.QueueShaderWrite(shader);
      shader_storage_flush_shaders_ = true;
    }
  }
  shader_storage_.QueuePipelineWrite(stored);
  shader_storage_flush_pipelines_ = true;
}

void MetalPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id) {
  ShutdownShaderStorage();
  static const bool s_enabled = metal::ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_SHADER_STORAGE"), true);
  if (!s_enabled) {
    return;
  }
  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig config;
  // Not ".metal.xpso": Xenia Edge's Metal backend writes that name in the
  // same storage root with a different record.
  config.file_suffix = ".msc.metal.xpso";
  config.api_magic = 0x4C544D58;  // 'XMTL'
  config.version = std::max(StoredPipelineDescription::kVersion,
                            ::rex::gpu::DXBCShaderTranslator::Modification::kVersion);
  const uint32_t storage_index = shader_storage_.storage_index() + 1;
  const auto start = std::chrono::steady_clock::now();
  std::vector<PipelineStoredDescription> stored_descriptions;
  if (!shader_storage_.InitializeShaderStorage(
          cache_root, title_id, config,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            if (MetalShader* shader =
                    LoadShader(type, ucode_dwords, ucode_dword_count)) {
              shader->set_ucode_storage_index(storage_index);
            }
            return true;
          },
          nullptr, stored_descriptions)) {
    return;
  }
  // Rebuild every stored pipeline. Each shader and pipeline goes through the
  // same cache-aware path a draw takes, so with warm caches this is metallib
  // and binary archive hits; after a translator change it is the compile the
  // session would otherwise do during play.
  //
  // That compile is spread over several threads: on one, Mafia II's 1314
  // pipelines took 39 s of loading on the M1 Max, three quarters of it
  // pipeline-state creation and the rest DXBC -> DXIL -> metallib. Everything
  // that changes the shaders, the caches or the binary archive stays on this
  // thread, before and after each parallel part.
  shader_storage_replaying_ = true;
  // XE_METAL_SHADER_STORAGE_THREADS=<n>: threads, this one included (1 = this
  // thread alone). Default 3/4 of the logical processors, as Vulkan does.
  static const uint32_t s_thread_count = [] {
    if (const char* value = std::getenv("XE_METAL_SHADER_STORAGE_THREADS")) {
      const long count = std::strtol(value, nullptr, 10);
      if (count > 0) {
        return uint32_t(count);
      }
    }
    return std::max(rex::thread::logical_processor_count() * 3 / 4,
                    uint32_t(1));
  }();
  string::StringBuffer ucode_disasm;
  auto find_shader = [&](uint64_t hash) -> MetalShader* {
    auto it = shaders_.find(hash);
    if (it == shaders_.end()) {
      return nullptr;
    }
    MetalShader* shader = it->second;
    if (!shader->IsUcodeAnalyzed()) {
      shader->AnalyzeUcode(ucode_disasm, std::string());
    }
    return shader;
  };
  struct ReplayPipeline {
    const StoredPipelineDescription* description;
    MetalShader::MetalTranslation* vertex;
    MetalShader::MetalTranslation* pixel;
    uint64_t key = 0;
    MTLRenderPipelineDescriptor* descriptor = nil;
    void* state = nullptr;  // Retained until AddRenderPipelineState.
    bool archive_stale = false;
  };
  std::vector<ReplayPipeline> pipelines;
  pipelines.reserve(stored_descriptions.size());
  std::vector<MetalShader::MetalTranslation*> translations;
  std::unordered_set<MetalShader::MetalTranslation*> translations_queued;
  auto queue_translation = [&](MetalShader::MetalTranslation* translation) {
    if (!translation->IsTranslated() &&
        translations_queued.insert(translation).second) {
      translations.push_back(translation);
    }
  };
  size_t built = 0, missing_shader = 0, failed = 0;
  for (const PipelineStoredDescription& stored : stored_descriptions) {
    const StoredPipelineDescription& description = stored.description;
    MetalShader* vertex_shader = find_shader(description.vertex_shader_hash);
    MetalShader* pixel_shader =
        description.pixel_shader_hash
            ? find_shader(description.pixel_shader_hash)
            : nullptr;
    if (!vertex_shader || (description.pixel_shader_hash && !pixel_shader)) {
      ++missing_shader;
      continue;
    }
    auto* vertex = static_cast<MetalShader::MetalTranslation*>(
        vertex_shader->GetOrCreateTranslation(
            description.vertex_shader_modification));
    queue_translation(vertex);
    MetalShader::MetalTranslation* pixel = nullptr;
    if (pixel_shader) {
      ::rex::gpu::DXBCShaderTranslator::Modification modification(
          description.pixel_shader_modification);
      if (description.pixel_fetch_pattern.fetch_mask) {
        // 0 when the shader's pattern table is full: the fully dynamic
        // translation, as a draw would get.
        modification.pixel.fetch_plain_pattern =
            pixel_shader->FindOrAddFetchPattern(
                description.pixel_fetch_pattern);
      }
      pixel = static_cast<MetalShader::MetalTranslation*>(
          pixel_shader->GetOrCreateTranslation(modification.value));
      queue_translation(pixel);
    }
    ReplayPipeline& pipeline = pipelines.emplace_back();
    pipeline.description = &description;
    pipeline.vertex = vertex;
    pipeline.pixel = pixel;
  }

  // Translation: a translator and a Metal Shader Converter per thread.
  struct TranslationWorker {
    std::unique_ptr<::rex::gpu::DXBCShaderTranslator> translator;
    std::unique_ptr<MetalShaderConverter> metal_converter;
  };
  std::vector<TranslationWorker> translation_workers(
      std::min(size_t(s_thread_count), translations.size()));
  for (size_t i = 1; i < translation_workers.size(); ++i) {
    TranslationWorker& worker = translation_workers[i];
    worker.translator = CreateShaderTranslator();
    worker.metal_converter = std::make_unique<MetalShaderConverter>();
    if (!worker.metal_converter->Initialize(
            MetalTranslatorConfig::kMscBindlessResourcesUsed)) {
      translation_workers.resize(i);
      break;
    }
  }
  RunShaderStorageWorkers(
      translations.size(), uint32_t(translation_workers.size()),
      [&](uint32_t worker_index, size_t item) {
        @autoreleasepool {
          TranslationWorker& worker = translation_workers[worker_index];
          if (worker.translator) {
            TranslateShader(*translations[item], *worker.translator,
                            *worker.metal_converter);
          } else {
            TranslateShader(*translations[item]);
          }
        }
      });

  // Pipeline states, for the pipelines whose shaders translated and that are
  // not cached already (two descriptions can make one pipeline).
  auto translated = [](const MetalShader::MetalTranslation* translation) {
    return translation->IsTranslated() && translation->IsValid() &&
           translation->has_metal_function();
  };
  std::vector<size_t> creations;
  std::unordered_set<uint64_t> keys_queued;
  for (size_t i = 0; i < pipelines.size(); ++i) {
    ReplayPipeline& pipeline = pipelines[i];
    const StoredPipelineDescription& description = *pipeline.description;
    if (!translated(pipeline.vertex) ||
        (pipeline.pixel && !translated(pipeline.pixel))) {
      continue;
    }
    pipeline.key = RenderPipelineKey(
        pipeline.vertex, pipeline.pixel, description.color_formats,
        description.depth_format, {description.sample_count},
        description.blend_controls, description.color_write_mask);
    if (!render_pipeline_states_.count(pipeline.key) &&
        keys_queued.insert(pipeline.key).second) {
      creations.push_back(i);
    }
  }
  RunShaderStorageWorkers(
      creations.size(), s_thread_count, [&](uint32_t, size_t item) {
        @autoreleasepool {
          ReplayPipeline& pipeline = pipelines[creations[item]];
          const StoredPipelineDescription& description = *pipeline.description;
          pipeline.descriptor = (__bridge_transfer MTLRenderPipelineDescriptor*)
              NewRenderPipelineDescriptor(
                  pipeline.vertex, pipeline.pixel, description.color_formats,
                  description.depth_format, {description.sample_count},
                  description.blend_controls, description.color_write_mask,
                  binary_archive_lookup_list_);
          pipeline.state = NewRenderPipelineState(
              (__bridge void*)pipeline.descriptor, pipeline.key,
              &pipeline.archive_stale);
        }
      });
  for (size_t i : creations) {
    ReplayPipeline& pipeline = pipelines[i];
    if (pipeline.archive_stale) {
      archived_pipeline_keys_.erase(pipeline.key);
    }
    AddRenderPipelineState(pipeline.key, (__bridge void*)pipeline.descriptor,
                           pipeline.state);
    pipeline.descriptor = nil;
  }
  for (const ReplayPipeline& pipeline : pipelines) {
    auto it = pipeline.key ? render_pipeline_states_.find(pipeline.key)
                           : render_pipeline_states_.end();
    if (it != render_pipeline_states_.end() && it->second) {
      ++built;
    } else {
      ++failed;
    }
  }
  shader_storage_replaying_ = false;
  REXLOG_INFO(
      "MetalPipelineCache: shader storage rebuilt {} of {} pipelines in {} ms "
      "({} missing a shader, {} failed; {} shaders translated, {} pipeline "
      "states created, {} threads)",
      built, stored_descriptions.size(),
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count(),
      missing_shader, failed, translations.size(), creations.size(),
      s_thread_count);
}

void MetalPipelineCache::RunShaderStorageWorkers(
    size_t item_count, uint32_t worker_count,
    const std::function<void(uint32_t worker, size_t item)>& work) {
  std::atomic<size_t> next_item{0};
  auto run = [&](uint32_t worker) {
    for (size_t item; (item = next_item.fetch_add(1)) < item_count;) {
      work(worker, item);
    }
  };
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  const uint32_t thread_count =
      uint32_t(std::min(size_t(std::max(worker_count, uint32_t(1))),
                        std::max(item_count, size_t(1))));
  for (uint32_t worker = 1; worker < thread_count; ++worker) {
    rex::thread::Thread::CreationParameters params;
    auto thread = rex::thread::Thread::Create(
        params, [&run, worker]() { run(worker); });
    if (!thread) {
      break;
    }
    thread->SetName("Metal Pipelines");
    threads.push_back(std::move(thread));
  }
  run(0);
  for (auto& thread : threads) {
    rex::thread::Wait(thread.get(), false);
  }
}

void MetalPipelineCache::ShutdownShaderStorage() {
  FlushShaderStorage();
  shader_storage_.ShutdownShaderStorage();
}

void MetalPipelineCache::FlushShaderStorage() {
  if (shader_storage_flush_shaders_ || shader_storage_flush_pipelines_) {
    shader_storage_.RequestFlush(shader_storage_flush_shaders_,
                                 shader_storage_flush_pipelines_);
    shader_storage_flush_shaders_ = false;
    shader_storage_flush_pipelines_ = false;
  }
}

void* MetalPipelineCache::NewLibraryFromBytes(const std::vector<uint8_t>& bytes,
                                              const char* label) {
  if (!device_ || bytes.empty()) {
    return nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  dispatch_data_t data = dispatch_data_create(
      bytes.data(), bytes.size(), dispatch_get_main_queue(),
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
#if !__has_feature(objc_arc)
  dispatch_release(data);
#endif
  if (!library) {
    REXLOG_ERROR("MetalPipelineCache: {} newLibraryWithData failed: {}",
           label ? label : "library",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return nullptr;
  }
  return (__bridge_retained void*)library;
}

const MetalPipelineCache::GeometryVertexStage*
MetalPipelineCache::GetOrCreateGeometryVertexStage(
    MetalShader::MetalTranslation* vertex, PipelineGeometryShader geometry_type) {
  // The object stage is compiled against the MSC input topology that
  // `geometry_type` selects (point -> kPoint, rect -> kTriangle, quad ->
  // kLine), so two geometry types produce DIFFERENT object stages for the same
  // guest VS. This cache used to key on `vertex` alone and would hand back
  // whichever was built first; the topology is now part of the key. Found while
  // building the durable key for 3.10, where persisting the same mistake would
  // have made it survive across runs instead of only within one.
  const MetalGeometryVertexStageKey stage_key{
      static_cast<const void*>(vertex),
      uint32_t(GeometryInputTopology(geometry_type))};
  auto it = geometry_vertex_stages_.find(stage_key);
  if (it != geometry_vertex_stages_.end()) {
    return it->second.valid ? &it->second : nullptr;
  }
  // Cache the result even on failure so a bad shader isn't retried every draw.
  GeometryVertexStage stage;

  // 3.10 — durable geometry record. Keyed on every input that affects object-
  // stage generation: the guest shader identity, its translation modification,
  // and the MSC input topology.
  const uint64_t record_key =
      MetalGeometryRecordKey(kMetalGeometryRecordKindVertexStage,
                        MetallibCacheKey(*vertex),
                        uint32_t(GeometryInputTopology(geometry_type)));
  if (::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() && storage_.is_open()) {
    MetalPipelineStorage::GeometryRecord record;
    if (storage_.LoadGeometryRecord(record_key, &record) &&
        !record.aux_blob.empty()) {
      void* library = NewLibraryFromBytes(record.blob, "geometry VS (cached)");
      void* stage_in =
          NewLibraryFromBytes(record.aux_blob, "geometry VS stage-in (cached)");
      if (library && stage_in) {
        stage.library = library;
        stage.stage_in_library = stage_in;
        stage.function_name = record.function_name;
        stage.vertex_output_size = record.reflection;
        stage.valid = true;
        auto inserted = geometry_vertex_stages_.emplace(stage_key, std::move(stage));
        return &inserted.first->second;
      }
      // Either library failing makes the record unusable as a whole — release
      // whichever loaded and fall through to a full cold build.
      if (library) {
        id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)library;
        (void)lib;
      }
      if (stage_in) {
        id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)stage_in;
        (void)lib;
      }
      REXLOG_WARN(
          "MetalPipelineCache: cached geometry VS record {:016X} failed to "
          "load; rebuilding",
          record_key);
    }
  }

  // Reuse the DXBC the normal VS translation already produced (the guest VS runs
  // as the object stage with host_vertex_shader_type kVertex — geometry emulation
  // does the primitive expansion in the GS/mesh stage, so the VS DXBC is
  // identical to the normal path; see XeniOS host-VS-type remap).
  const std::vector<uint8_t>& dxbc_source = vertex->GetTranslatedBinary();
  if (dxbc_source.empty()) {
    REXLOG_ERROR("MetalPipelineCache: geometry VS has no DXBC (translate VS first)");
    geometry_vertex_stages_.emplace(stage_key, std::move(stage));
    return nullptr;
  }
  // Clip-distance semantic legalization, UNCONDITIONAL. Relabels the
  // clip-distance semantic on the OBJECT STAGE'S OUTPUT so it crosses the
  // object->mesh hop as a private user parameter; without it a geometry-
  // emulation pipeline whose VS writes SV_ClipDistance fails to create and the
  // whole draw class is silently dropped.
  // Inert for clip-count-0 shaders: proven BYTE-IDENTICAL in and out by the
  // transport oracle, which is what makes running it on every geometry stage
  // safe rather than merely convenient.
  std::vector<uint8_t> dxbc = dxbc_source;
  {
    uint32_t n = RelabelDxbcClipDistance(dxbc, DxbcSignatureKind::kOutput);
    if (n) {
      REXLOG_INFO("MetalPipelineCache: clip relabel — object-stage output, {} element(s)", n);
    }
  }
  std::string error;
  std::vector<uint8_t> dxil;
  // Cache-aware: the key is the POST-RELABEL buffer above, which no
  // shader-level key could see.
  if (!ConvertDxbcToDxilCached(dxbc, dxil, &error, "geometry-object")) {
    REXLOG_ERROR("MetalPipelineCache: geometry VS DXBC->DXIL failed: {}", error);
    geometry_vertex_stages_.emplace(stage_key, std::move(stage));
    return nullptr;
  }
  MetalShaderConverter::GeometryEmulationResult result;
  MetalStageRequestProbe geom_vs_probe(
      dxil, uint32_t(MetalShaderConverter::Stage::kVertex),
      uint32_t(GeometryInputTopology(geometry_type)));
  if (!metal_converter_.ConvertForGeometryEmulation(
          MetalShaderConverter::Stage::kVertex, dxil,
          GeometryInputTopology(geometry_type), result)) {
    REXLOG_ERROR("MetalPipelineCache: geometry VS emulation convert failed: {}",
           result.error.empty() ? "unknown" : result.error);
    geometry_vertex_stages_.emplace(stage_key, std::move(stage));
    return nullptr;
  }
  void* library = NewLibraryFromBytes(result.metallib, "geometry VS");
  void* stage_in =
      NewLibraryFromBytes(result.stage_in_metallib, "geometry VS stage-in");
  if (!library || !stage_in) {
    if (library) {
      id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)library;
      (void)lib;
    }
    if (stage_in) {
      id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)stage_in;
      (void)lib;
    }
    geometry_vertex_stages_.emplace(stage_key, std::move(stage));
    return nullptr;
  }
  stage.library = library;
  stage.stage_in_library = stage_in;
  stage.function_name = result.function_name;
  stage.vertex_output_size = result.vertex_output_size_in_bytes;
  stage.valid = true;
  if (::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() && storage_.is_open() &&
      stage.vertex_output_size) {
    MetalPipelineStorage::GeometryRecord record;
    record.function_name = stage.function_name;
    record.reflection = stage.vertex_output_size;
    record.blob = result.metallib;
    record.aux_blob = result.stage_in_metallib;
    storage_.StoreGeometryRecord(record_key, record);
  }
  auto inserted = geometry_vertex_stages_.emplace(stage_key, std::move(stage));
  return &inserted.first->second;
}

const MetalPipelineCache::GeometryShaderStage*
MetalPipelineCache::GetOrCreateGeometryShaderStage(GeometryShaderKey key) {
  auto it = geometry_shader_stages_.find(key.key);
  if (it != geometry_shader_stages_.end()) {
    return it->second.valid ? &it->second : nullptr;
  }
  GeometryShaderStage stage;

  // 3.10 — durable geometry record. The generated GS is fully determined by
  // GeometryShaderKey (topology type plus the interpolator/clip layout derived
  // from the VS/PS modifications), so that key IS the identity.
  const uint64_t record_key =
      MetalGeometryRecordKey(kMetalGeometryRecordKindShaderStage, key.key, 0);
  if (::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() && storage_.is_open()) {
    MetalPipelineStorage::GeometryRecord record;
    if (storage_.LoadGeometryRecord(record_key, &record)) {
      void* library = NewLibraryFromBytes(record.blob, "geometry GS (cached)");
      if (library) {
        stage.library = library;
        stage.function_name = record.function_name;
        stage.max_input_primitives = record.reflection;
        stage.valid = true;
        auto inserted =
            geometry_shader_stages_.emplace(key.key, std::move(stage));
        return &inserted.first->second;
      }
      REXLOG_WARN(
          "MetalPipelineCache: cached geometry GS record {:016X} failed to "
          "load; rebuilding",
          record_key);
    }
  }

  // Generate the DXBC geometry shader (shared point/rect/quad generator), then
  // run it through the same DXBC->DXIL->MSC chain as a MESH stage.
  std::vector<uint32_t> dxbc_dwords;
  CreateDxbcGeometryShader(key, dxbc_dwords);
  if (dxbc_dwords.empty()) {
    REXLOG_ERROR("MetalPipelineCache: geometry shader generator produced no DXBC");
    geometry_shader_stages_.emplace(key.key, std::move(stage));
    return nullptr;
  }
  std::vector<uint8_t> dxbc_bytes(dxbc_dwords.size() * sizeof(uint32_t));
  std::memcpy(dxbc_bytes.data(), dxbc_dwords.data(), dxbc_bytes.size());
  // Matching relabel on the MESH STAGE'S INPUT. Its rasterizer-facing OUTPUT
  // deliberately keeps SV_ClipDistance, which is what makes the hardware clip.
  // Unconditional; inert byte-for-byte when the shader has no clip distances.
  {
    {
      uint32_t n =
          RelabelDxbcClipDistance(dxbc_bytes, DxbcSignatureKind::kInput);
      if (n) {
        REXLOG_INFO("MetalPipelineCache: clip relabel — mesh-stage input, {} element(s)", n);
      }
    }
  }

  std::string error;
  std::vector<uint8_t> dxil;
  // Cache-aware; same post-relabel keying rule as the object stage.
  if (!ConvertDxbcToDxilCached(dxbc_bytes, dxil, &error, "geometry-mesh")) {
    REXLOG_ERROR("MetalPipelineCache: geometry GS DXBC->DXIL failed: {}", error);
    geometry_shader_stages_.emplace(key.key, std::move(stage));
    return nullptr;
  }
  MetalShaderConverter::GeometryEmulationResult result;
  MetalStageRequestProbe geom_gs_probe(
      dxil, uint32_t(MetalShaderConverter::Stage::kGeometry),
      uint32_t(GeometryInputTopology(
          static_cast<PipelineGeometryShader>(key.type))));
  if (!metal_converter_.ConvertForGeometryEmulation(
          MetalShaderConverter::Stage::kGeometry, dxil,
          GeometryInputTopology(static_cast<PipelineGeometryShader>(key.type)),
          result) ||
      (!result.has_mesh_stage && !result.has_geometry_stage)) {
    REXLOG_ERROR("MetalPipelineCache: geometry GS emulation convert failed: {}",
           result.error.empty() ? "no mesh/geometry stage" : result.error);
    geometry_shader_stages_.emplace(key.key, std::move(stage));
    return nullptr;
  }
  void* library = NewLibraryFromBytes(result.metallib, "geometry GS");
  if (!library) {
    geometry_shader_stages_.emplace(key.key, std::move(stage));
    return nullptr;
  }
  stage.library = library;
  stage.function_name = result.function_name;
  stage.max_input_primitives = result.gs_max_input_primitives_per_mesh_threadgroup;
  stage.valid = true;
  if (::rex::graphics::metal::GetMetalSettings().metal_pipeline_metallib_cache.Get() && storage_.is_open() &&
      stage.max_input_primitives) {
    MetalPipelineStorage::GeometryRecord record;
    record.function_name = stage.function_name;
    record.reflection = stage.max_input_primitives;
    record.blob = result.metallib;  // No stage-in for a mesh stage.
    storage_.StoreGeometryRecord(record_key, record);
  }
  auto inserted = geometry_shader_stages_.emplace(key.key, std::move(stage));
  return &inserted.first->second;
}

void* MetalPipelineCache::GetOrCreateGeometryPipelineState(
    MetalShader::MetalTranslation* vertex, MetalShader::MetalTranslation* pixel,
    PipelineGeometryShader geometry_type,
    ::rex::gpu::DXBCShaderTranslator::Modification vertex_modification,
    ::rex::gpu::DXBCShaderTranslator::Modification pixel_modification,
    const uint64_t* color_pixel_formats, uint64_t depth_pixel_format,
    MetalPipelineRasterKey raster_key, const uint32_t* blend_controls,
    uint32_t color_write_mask,
    uint32_t* gs_vertex_size_out, uint32_t* gs_max_input_primitives_out) {
  if (gs_vertex_size_out) {
    *gs_vertex_size_out = 0;
  }
  if (gs_max_input_primitives_out) {
    *gs_max_input_primitives_out = 0;
  }
  // The geometry-emulation pipeline requires a fragment stage (the descriptor
  // dereferences fragmentLibrary); a depth-only geometry path isn't wired yet.
  if (!vertex || !pixel || !pixel->has_metal_function() ||
      geometry_type == PipelineGeometryShader::kNone) {
    return nullptr;
  }

  // The generated GS is keyed by the interpolator/clip layout derived from the
  // VS/PS modifications; returns false if this type needs no geometry shader.
  GeometryShaderKey gs_key;
  if (!GetGeometryShaderKey(geometry_type, vertex_modification,
                            pixel_modification, gs_key)) {
    return nullptr;
  }

  // 3.2 — same content-derived identity as the render-pipeline key. `gs_key` is
  // already content-derived (topology type plus the interpolator/clip layout
  // taken from the VS/PS modifications), so it needs no change.
  GeometryPipelineKeyBytes key_bytes = {};
  key_bytes.gs_key = gs_key.key;
  key_bytes.vs = MakeShaderKeyIdentity(vertex);
  key_bytes.ps = MakeShaderKeyIdentity(pixel);
  key_bytes.depth = depth_pixel_format;
  key_bytes.sample_count = raster_key.raster_sample_count;
  key_bytes.write_mask = color_write_mask;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    key_bytes.colors[i] = color_pixel_formats[i];
    key_bytes.blends[i] = blend_controls[i];
  }
  uint64_t key = XXH3_64bits(&key_bytes, sizeof(key_bytes));
  auto cache_it = geometry_pipelines_.find(key);
  if (cache_it != geometry_pipelines_.end()) {
    if (!cache_it->second.pipeline) {
      // A cached negative is not "one failure": every hit here is a DRAW the
      // failed creation is still suppressing. Count them separately from
      // creation attempts so impact is visible; log the first suppression and
      // then powers of two, joined to the failure line by the key digest.
      uint32_t count = ++cache_it->second.negative_hits;
      if ((count & (count - 1)) == 0) {
        REXLOG_WARN(
            "MetalPipelineCache: geometry pipeline negative-cache hit #{} "
            "(draws suppressed by one failed creation) key={:016X}",
            count, key);
      }
      return nullptr;
    }
    if (gs_vertex_size_out) {
      *gs_vertex_size_out = cache_it->second.gs_vertex_size;
    }
    if (gs_max_input_primitives_out) {
      *gs_max_input_primitives_out = cache_it->second.gs_max_input_primitives;
    }
    return cache_it->second.pipeline;
  }

  const GeometryVertexStage* vstage =
      GetOrCreateGeometryVertexStage(vertex, geometry_type);
  const GeometryShaderStage* gstage = GetOrCreateGeometryShaderStage(gs_key);
  if (!vstage || !gstage || !vstage->vertex_output_size ||
      !gstage->max_input_primitives) {
    REXLOG_ERROR(
        "MetalPipelineCache: geometry pipeline missing stage/reflection "
        "(vs_output={}, gs_max_input={})",
        vstage ? vstage->vertex_output_size : 0,
        gstage ? gstage->max_input_primitives : 0);
    geometry_pipelines_.emplace(key, GeometryPipeline{});
    return nullptr;
  }

  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLMeshRenderPipelineDescriptor* desc =
      [[MTLMeshRenderPipelineDescriptor alloc] init];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (color_pixel_formats[i] == MTLPixelFormatInvalid) {
      continue;
    }
    desc.colorAttachments[i].pixelFormat =
        (MTLPixelFormat)color_pixel_formats[i];
    ApplyRtBlendState(desc.colorAttachments[i], blend_controls[i],
                      (color_write_mask >> (i * 4u)) & 0xFu);
  }
  {
    const MTLPixelFormat depth_format =
        (MTLPixelFormat)(depth_pixel_format & ~kDepthWithoutStencilBit);
    if (depth_format != MTLPixelFormatInvalid) {
      desc.depthAttachmentPixelFormat = depth_format;
      if (depth_format == MTLPixelFormatDepth32Float_Stencil8 &&
          !(depth_pixel_format & kDepthWithoutStencilBit)) {
        desc.stencilAttachmentPixelFormat = depth_format;
      }
    }
  }
  desc.rasterSampleCount = raster_key.raster_sample_count;
  // The fragment function writes the complete Xenos coverage mask dynamically.
  // Keep the host coverage filter disabled for the same reason as the regular
  // render-pipeline path above.
  desc.alphaToCoverageEnabled = NO;

  id<MTLFunction> ps_fn = (__bridge id<MTLFunction>)pixel->metal_function();
  std::string pixel_function_name = ps_fn.name ? [ps_fn.name UTF8String] : "";

  IRGeometryEmulationPipelineDescriptor ir_desc = {};
  ir_desc.stageInLibrary = (__bridge id<MTLLibrary>)vstage->stage_in_library;
  ir_desc.vertexLibrary = (__bridge id<MTLLibrary>)vstage->library;
  ir_desc.vertexFunctionName = vstage->function_name.c_str();
  ir_desc.geometryLibrary = (__bridge id<MTLLibrary>)gstage->library;
  ir_desc.geometryFunctionName = gstage->function_name.c_str();
  ir_desc.fragmentLibrary = (__bridge id<MTLLibrary>)pixel->metal_library();
  ir_desc.fragmentFunctionName = pixel_function_name.c_str();
  ir_desc.basePipelineDescriptor = desc;
  ir_desc.pipelineConfig.gsVertexSizeInBytes = vstage->vertex_output_size;
  ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup =
      gstage->max_input_primitives;

  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline = nil;
  {
    // Task 1.1 census: the geometry-emulation mesh pipeline is a separate
    // stage from the plain PSO — it compiles an object+mesh+fragment triple
    // through the IR runtime — so folding the two would make a title that uses
    // geometry emulation look like one with expensive ordinary pipelines.
    MetalShaderBuildTimer timer(GetMetalShaderBuildCensus().geometry_pso);
    pipeline = IRRuntimeNewGeometryEmulationPipeline(device, &ir_desc, &error);
    if (!pipeline) {
      timer.Dismiss();
    }
  }
  if (!pipeline) {
    // Report the SAME identifying fields the success path logs. Without them a
    // failure is an unattributable string: it names neither which geometry type
    // failed, nor which guest shaders, nor the two pipeline-config values that
    // are the usual cause of a converter rejection. A negative cached below is
    // permanent for the run, so a draw class is silently dropped for the rest
    // of the session — that must be diagnosable from one log.
    REXLOG_ERROR(
        "MetalPipelineCache: geometry pipeline creation failed: {} "
        "(type={}, gs_vertex_size={}, gs_max_input_primitives={}, "
        "vs_fn='{}', gs_fn='{}', ps_fn='{}') {}",
        error ? [[error localizedDescription] UTF8String] : "unknown",
        uint32_t(geometry_type), vstage->vertex_output_size,
        gstage->max_input_primitives, vstage->function_name,
        gstage->function_name, pixel_function_name,
        DescribeGeometryPipelineKey(key, key_bytes));
    geometry_pipelines_.emplace(key, GeometryPipeline{});
    return nullptr;
  }

  REXLOG_INFO(
      "MetalPipelineCache: built geometry pipeline (type={}, gs_vertex_size={}, "
      "gs_max_input_primitives={}, vs_fn='{}', gs_fn='{}') {}",
      uint32_t(geometry_type), vstage->vertex_output_size,
      gstage->max_input_primitives, vstage->function_name,
      gstage->function_name, DescribeGeometryPipelineKey(key, key_bytes));

  GeometryPipeline state;
  state.pipeline = (__bridge_retained void*)pipeline;
  state.gs_vertex_size = vstage->vertex_output_size;
  state.gs_max_input_primitives = gstage->max_input_primitives;
  if (gs_vertex_size_out) {
    *gs_vertex_size_out = state.gs_vertex_size;
  }
  if (gs_max_input_primitives_out) {
    *gs_max_input_primitives_out = state.gs_max_input_primitives;
  }
  auto inserted = geometry_pipelines_.emplace(key, std::move(state));
  return inserted.first->second.pipeline;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
