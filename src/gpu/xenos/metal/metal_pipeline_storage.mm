/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <fmt/format.h>
#include "rex/graphics/metal/metal_pipeline_storage.h"

#include "rex/graphics/metal/metal_draw_resolution_scale.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ctime>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xxhash.h"
#include <rex/graphics/metal/metal_settings.h>
#include <rex/logging/log_category.h>

#include "rex/graphics/metal/metal_converter_options.h"
#include "rex/graphics/metal/metal_dxil_store.h"
#include "rex/ui/metal/metal_provider.h"

// 3.6b — size-based bounds, configurable. Not age-based: a large title touched
// rarely is the thing worth evicting, and its age says nothing about its cost.
// One exact rollback switch per mechanism, so a regression bisects to one
// mechanism rather than to "the durable cache". M1 is PROMOTED (default on);
// M2 remains default off pending its own successor change and gates.
// Config migration for the promotion: resets only configs still holding the
// old default (false). The flag shipped default-off for one day (53b2676f5),
// so a false in a config is that default, not an opt-out.

namespace rex {
namespace graphics {
namespace metal {

MetalGammaRenderTargetLinearSetting ParseMetalGammaRenderTargetLinearSetting(
    const char* value) {
  if (!value || !std::strcmp(value, "1")) {
    return MetalGammaRenderTargetLinearSetting::kEnabled;
  }
  if (!std::strcmp(value, "0")) {
    return MetalGammaRenderTargetLinearSetting::kDisabled;
  }
  return MetalGammaRenderTargetLinearSetting::kMalformed;
}

bool MetalGammaRenderTargetLinearEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XE_METAL_GAMMA_RT_LINEAR");
    const MetalGammaRenderTargetLinearSetting setting =
        ParseMetalGammaRenderTargetLinearSetting(value);
    if (setting == MetalGammaRenderTargetLinearSetting::kMalformed) {
      REXLOG_WARN(
          "XE_METAL_GAMMA_RT_LINEAR='{}' is malformed; expected exact 0 or "
          "1. Rolling back to legacy gamma render-target storage.",
          value);
    }
    return setting == MetalGammaRenderTargetLinearSetting::kEnabled;
  }();
  return enabled;
}

namespace {

constexpr char kManifestName[] = "identity.manifest";
constexpr char kLockName[] = ".lock";
constexpr char kMetallibDirName[] = "metallib";
constexpr char kBinaryArchiveName[] = "archive";
constexpr uint32_t kManifestMagic = 0x584D5053u;  // 'XMPS'

// On-disk manifest. Fixed layout; every field participates in the trust
// decision. last_access_seconds is 3.6c's per-launch access record — filesystem
// atime is commonly disabled or relatime, so it cannot be relied on.
#pragma pack(push, 1)
struct ManifestFile {
  uint32_t magic;
  uint32_t format_version;
  uint32_t translator_schema;
  uint32_t title_id;
  uint64_t dxc_artifact_hash;
  uint64_t msc_artifact_hash;
  uint64_t device_hash;
  uint64_t os_build_hash;
  int64_t last_access_seconds;
  // Was `reserved` (always written 0, never validated). Now carries the
  // dxilconv option-string hash, which is a trust field like the ones above.
  // Occupying the reserved slot keeps the record size and every other field
  // offset unchanged; a manifest written before this field existed has 0 here
  // while the running build hashes a non-empty option string, so the first
  // launch after this change discards the directory and rebuilds once. That is
  // the correct outcome: those entries were written under an option string
  // nothing recorded.
  uint64_t converter_options_hash;
};
#pragma pack(pop)

uint64_t HashString(const std::string& value) {
  return value.empty() ? 0ull : XXH3_64bits(value.data(), value.size());
}

// 3.3c — hash the ACTUAL translator binary, not a version string it advertises.
// Both translators are dylibs (libdxilconv.dylib, libmetalirconverter.dylib),
// so the loaded image list names them directly. The image's Mach-O LC_UUID
// identifies the build itself: the linker gives every link a new one, so a
// locally patched build differs from a released one (the case that actually
// applies: thirdparty/DirectXShaderCompiler carries an uncommitted local
// patch, so no released version string could tell the two apart).
//
// The identity used to be path + size + mtime, which discarded every title's
// cache whenever the app moved or was copied (a new install location, an
// update shipping the same dylibs, each test kit): the first session after
// that re-translated every shader inside live frames -- RDR on the Mac Pro
// 7.4 s of stalls, single freezes of 2-3 s, against 0.25 s with the cache.
// Path + size + mtime remains the fallback for an image without LC_UUID.
//
// Returns 0 when the image is not found, and 0 participates in equality — an
// entry written while a translator was unresolvable is not served once it
// becomes resolvable.
bool LoadedImageUuid(uint32_t image_index, uint8_t uuid_out[16]) {
  const mach_header* header = _dyld_get_image_header(image_index);
  if (!header) {
    return false;
  }
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(header);
  uint32_t ncmds = header->ncmds;
  if (header->magic == MH_MAGIC_64) {
    cursor += sizeof(mach_header_64);
  } else if (header->magic == MH_MAGIC) {
    cursor += sizeof(mach_header);
  } else {
    return false;
  }
  for (uint32_t i = 0; i < ncmds; ++i) {
    const auto* command = reinterpret_cast<const load_command*>(cursor);
    if (command->cmd == LC_UUID) {
      std::memcpy(uuid_out, reinterpret_cast<const uuid_command*>(command)->uuid,
                  16);
      return true;
    }
    if (!command->cmdsize) {
      return false;
    }
    cursor += command->cmdsize;
  }
  return false;
}

uint64_t HashLoadedDylib(const char* name_fragment) {
  const uint32_t image_count = _dyld_image_count();
  for (uint32_t i = 0; i < image_count; ++i) {
    const char* image_name = _dyld_get_image_name(i);
    if (!image_name || !std::strstr(image_name, name_fragment)) {
      continue;
    }
    uint8_t uuid[16];
    if (LoadedImageUuid(i, uuid)) {
      // Salted so a UUID hash never equals a path-based one by accident.
      return XXH3_64bits(uuid, sizeof(uuid)) ^ 0x55554944ull;
    }
    struct stat st = {};
    if (stat(image_name, &st) != 0) {
      return 0;
    }
    struct {
      uint64_t size;
      int64_t mtime;
      int64_t mtime_ns;
    } data = {static_cast<uint64_t>(st.st_size), st.st_mtimespec.tv_sec,
              st.st_mtimespec.tv_nsec};
    return HashString(image_name) ^
           (XXH3_64bits(&data, sizeof(data)) * 0x9E3779B97F4A7C15ull);
  }
  return 0;
}

uint64_t DirectorySizeBytes(const std::filesystem::path& dir) {
  std::error_code ec;
  uint64_t total = 0;
  auto it = std::filesystem::recursive_directory_iterator(
      dir, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return 0;
  }
  for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code file_ec;
    if (it->is_regular_file(file_ec) && !file_ec) {
      auto size = it->file_size(file_ec);
      if (!file_ec) {
        total += size;
      }
    }
  }
  return total;
}

int64_t NowSeconds() {
  return static_cast<int64_t>(time(nullptr));
}

}  // namespace

bool MetalPipelineStorageIdentity::operator==(
    const MetalPipelineStorageIdentity& other) const {
  return format_version == other.format_version &&
         translator_schema == other.translator_schema &&
         dxc_artifact_hash == other.dxc_artifact_hash &&
         msc_artifact_hash == other.msc_artifact_hash &&
         device_hash == other.device_hash &&
         os_build_hash == other.os_build_hash &&
         converter_options_hash == other.converter_options_hash;
}

std::string MetalPipelineStorageIdentity::DescribeMismatch(
    const MetalPipelineStorageIdentity& other) const {
  std::string out;
  auto add = [&out](const char* name) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  };
  if (format_version != other.format_version) add("format version");
  if (translator_schema != other.translator_schema) add("translator schema");
  if (dxc_artifact_hash != other.dxc_artifact_hash) add("DXC artifact");
  if (msc_artifact_hash != other.msc_artifact_hash) add("MSC artifact");
  if (device_hash != other.device_hash) add("device");
  if (os_build_hash != other.os_build_hash) add("OS build");
  if (converter_options_hash != other.converter_options_hash) {
    add("dxilconv options");
  }
  return out.empty() ? "none" : out;
}

bool MetalDeferredKillEnabled() {
  static const bool s_enabled = [] {
    if (const char* v = std::getenv("XE_METAL_DEFERRED_KILL")) {
      return v[0] == '1';
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();  // a family check, not a device-specific operation
    return device && ![device supportsFamily:MTLGPUFamilyApple7];
  }();
  return s_enabled;
}

MetalPipelineStorageIdentity ComputeMetalPipelineStorageIdentity(
    void* device, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y) {
  MetalPipelineStorageIdentity identity;

  // 3.3b — read from MetalTranslatorConfig, the same constants
  // MetalPipelineCache::Initialize constructs the translators from, so the
  // schema cannot drift from the configuration it describes.
  struct SchemaBits {
    uint32_t vendor_apple : 1;
    uint32_t bindless_resources_used : 1;
    uint32_t edram_rov_used : 1;
    uint32_t gamma_render_target_as_unorm8 : 1;
    uint32_t msc_bindless_resources_used : 1;
    uint32_t fetch_abi_revision : 8;
    uint32_t dxilconv_generation : 4;
    // Draw resolution scale minus 1 per axis (implement-metal-draw-resolution-
    // scaling): the translator bakes the scale into pixel position, texture
    // offset and memexport code. 0 at 1x, so 1x identities are unchanged.
    uint32_t draw_resolution_scale_x_minus_1 : 2;
    uint32_t draw_resolution_scale_y_minus_1 : 2;
    // --mulsc_round_toward_zero changes the emitted mulsc code.
    uint32_t mulsc_round_toward_zero : 1;
    // Deferred pixel-shader kill changes the emitted kill code.
    uint32_t deferred_kill : 1;
    // --dxbc_implicit_lod changes the emitted texture sampling (3 = the
    // snap-aware form keyed on mip count).
    uint32_t implicit_lod : 2;
    // --texture_point_snap changes the emitted texture coordinates.
    uint32_t texture_point_snap : 1;
    uint32_t reserved : 6;
  } schema = {};
  schema.vendor_apple = 1;
  schema.bindless_resources_used = MetalTranslatorConfig::kBindlessResourcesUsed;
  schema.edram_rov_used = MetalTranslatorConfig::kEdramRovUsed;
  schema.gamma_render_target_as_unorm8 =
      MetalTranslatorConfig::GammaRenderTargetAsUnorm8();
  schema.msc_bindless_resources_used =
      MetalTranslatorConfig::kMscBindlessResourcesUsed;
  // 3.3b-2 — the fetch ABI revision. The five booleans above describe the
  // translator's CONFIGURATION; they do not move when the emitted code for
  // identical ucode changes for another reason. The integer-texture-scale work
  // added a system constant and a fetch-time codepath without touching any of
  // them, so a cache written before it would have been served shaders compiled
  // against the previous cbuffer layout — exactly the failure this field
  // exists to prevent. Bump on ANY change to the emitted shader ABI.
  schema.fetch_abi_revision = MetalTranslatorConfig::kFetchAbiRevision;
  // The XE_GPU_VFETCH_BOUNDS=0 diagnostic emits different DXBC; keep its
  // records apart from the shipping revision.
  {
    const char* v = std::getenv("XE_GPU_VFETCH_BOUNDS");
    if (v && v[0] == '0') {
      schema.fetch_abi_revision += 64;
    }
  }
  // 3.3b-3 — the DXBC->DXIL conversion generation. Generation 1 = the
  // 2026-08-02 fresh-IDxbcConverter-per-convert fix: reused converter
  // instances carried call history and emitted CORRUPT quad-domain artifacts
  // (runtime-nondeterministic). The dylib hash below cannot see this (the bug
  // was in xenia's wrapper, not libdxilconv), and the metallib key carries no
  // DXIL bytes — so without this bump a warm cache would serve reuse-era
  // corruption indefinitely. Bump on any wrapper-side change to how
  // conversions are produced. The value lives in MetalTranslatorConfig next to
  // kFetchAbiRevision so the conformance mirror can name it rather than repeat
  // a literal that would drift silently.
  schema.dxilconv_generation = MetalTranslatorConfig::kDxilconvGeneration;
  schema.mulsc_round_toward_zero = GetMetalSettings().mulsc_round_toward_zero.Get() ? 1 : 0;
  schema.deferred_kill = MetalDeferredKillEnabled() ? 1 : 0;
  schema.implicit_lod = GetMetalSettings().dxbc_implicit_lod.Get() ? 3 : 0;
  schema.texture_point_snap = GetMetalSettings().texture_point_snap.Get() ? 1 : 0;
  schema.draw_resolution_scale_x_minus_1 =
      std::max(draw_resolution_scale_x, uint32_t(1)) - 1;
  schema.draw_resolution_scale_y_minus_1 =
      std::max(draw_resolution_scale_y, uint32_t(1)) - 1;
  // The exact scaled pixel position changes the pixel position the translator
  // emits, at a scale only (1x encodes as before).
  if ((draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) &&
      MetalExactScaledPixelPositionEnabled()) {
    schema.fetch_abi_revision += 128;
  }
  // A value wider than its bitfield does not fail to compile — it TRUNCATES,
  // and a truncated revision aliases an older one onto a newer build: exactly
  // the stale-serve these fields exist to prevent, arriving through the guard
  // itself. Pinned here rather than in a test because the failure has to be
  // impossible to build, not merely detectable at run time.
  static_assert(MetalTranslatorConfig::kFetchAbiRevision < (1u << 8),
                "kFetchAbiRevision no longer fits its 8-bit schema field; "
                "widen the field (and bump kFormatVersion) before bumping "
                "past 255, or the revision aliases an earlier one");
  static_assert(MetalTranslatorConfig::kDxilconvGeneration < (1u << 4),
                "kDxilconvGeneration no longer fits its 4-bit schema field; "
                "widen the field (and bump kFormatVersion) before bumping "
                "past 15, or the generation aliases an earlier one");
  static_assert(kMetalMaxDrawResolutionScale - 1 < (1u << 2),
                "The maximum draw resolution scale no longer fits its 2-bit "
                "schema fields");
  static_assert(sizeof(SchemaBits) == sizeof(identity.translator_schema),
                "SchemaBits must exactly fill translator_schema");
  std::memcpy(&identity.translator_schema, &schema, sizeof(schema));

  identity.dxc_artifact_hash = HashLoadedDylib("libdxilconv");
  identity.msc_artifact_hash = HashLoadedDylib("libmetalirconverter");
  if (!identity.dxc_artifact_hash || !identity.msc_artifact_hash) {
    REXLOG_WARN(
        "MetalPipelineStorage: could not fingerprint a translator dylib "
        "(dxc={:016X}, msc={:016X}) — the durable cache will still key on it, "
        "so a later resolvable build discards rather than reuses",
        identity.dxc_artifact_hash, identity.msc_artifact_hash);
  }

  // 2.1 item 3 — HOW dxilconv is invoked, not just which dylib is loaded. The
  // option string is read from the same header the converter reads, so the two
  // cannot disagree about what this session actually passed to Convert.
  //
  // The prefix is load-bearing: HashString returns 0 for an empty input, and 0
  // is also what a manifest written BEFORE this field existed carries in that
  // slot. Without the prefix, running with XENIA_DXBC2DXIL_FLAGS="" would match
  // every pre-field manifest and be served artifacts built under the DEFAULT
  // option string — the stale-serve this field exists to close, arriving
  // through its own encoding. With it, no option value can hash to 0, so a
  // pre-field manifest always mismatches and is discarded exactly once.
  identity.converter_options_hash =
      HashString("dxilconv-options:" + MetalConverterIdentityOptions());

  if (device) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
    std::string device_desc;
    if (mtl_device.name) {
      device_desc += [mtl_device.name UTF8String];
    }
    // lowPower is a macOS-only property; on iOS the cache key leaves it at the neutral value rather than failing to
    // compile, and the rest of the description still distinguishes devices.
#if TARGET_OS_OSX
    const int low_power = mtl_device.lowPower ? 1 : 0;
#else
    const int low_power = -1;
#endif
    device_desc += fmt::format(
        "|rid={}|lp={}|uma={}", uint64_t(mtl_device.registryID), low_power,
        mtl_device.hasUnifiedMemory ? 1 : 0);
    // GPU family participates because a metallib is compiled against it.
    for (NSInteger family = MTLGPUFamilyApple1; family <= MTLGPUFamilyApple9;
         ++family) {
      if ([mtl_device supportsFamily:(MTLGPUFamily)family]) {
        device_desc += fmt::format("|a{}", int(family - MTLGPUFamilyApple1 + 1));
      }
    }
    identity.device_hash = HashString(device_desc);
  }

  // The OS BUILD, not the marketing version: a build bump can change the
  // shader compiler without changing "26.5.2".
  @autoreleasepool {
    NSProcessInfo* info = [NSProcessInfo processInfo];
    std::string os_desc;
    if (info.operatingSystemVersionString) {
      os_desc = [info.operatingSystemVersionString UTF8String];
    }
    identity.os_build_hash = HashString(os_desc);
  }

  return identity;
}

uint64_t MetalGeometryRecordKey(uint32_t kind, uint64_t shader_identity,
                                uint32_t input_topology) {
  struct KeyBytes {
    uint32_t kind;
    uint32_t input_topology;
    uint64_t shader_identity;
  } key_bytes = {kind, input_topology, shader_identity};
  return XXH3_64bits(&key_bytes, sizeof(key_bytes));
}

MetalPipelineStorage::~MetalPipelineStorage() { Close(); }

std::filesystem::path MetalPipelineStorage::metallib_dir() const {
  return title_root_.empty() ? std::filesystem::path()
                             : title_root_ / kMetallibDirName;
}

std::filesystem::path MetalPipelineStorage::binary_archive_path() const {
  return title_root_.empty() ? std::filesystem::path()
                             : title_root_ / kBinaryArchiveName;
}

bool MetalPipelineStorage::Open(
    const std::filesystem::path& cache_root, uint32_t title_id,
    const MetalPipelineStorageIdentity& identity) {
  Close();
  if (cache_root.empty()) {
    return false;
  }

  // 3.6a — numeric title id only, never a game name.
  std::filesystem::path metal_root = cache_root / "metal";
  title_root_ = metal_root / fmt::format("{:08X}", title_id);
  title_id_ = title_id;

  std::error_code ec;
  std::filesystem::create_directories(title_root_, ec);
  if (ec) {
    REXLOG_WARN("MetalPipelineStorage: cannot create {}: {} — no durable storage",
           title_root_.string(), ec.message());
    title_root_.clear();
    return false;
  }

  // 3.6d — lock BEFORE eviction considers anything, so this title can never be
  // a candidate for its own eviction and a concurrent instance is visible.
  // Concurrent instances are not hypothetical: a stray second emulator
  // contaminated two census samples.
  if (!AcquireLock()) {
    REXLOG_WARN(
        "MetalPipelineStorage: {} is held by another process — no durable "
        "storage this session",
        title_root_.string());
    title_root_.clear();
    return false;
  }

  std::string mismatch;
  if (!LoadAndValidateManifest(identity, &mismatch)) {
    // 3.4 — discard and rebuild rather than partially trusting. Logged, because
    // a silent rebuild is indistinguishable from a cache that never worked.
    REXLOG_INFO(
        "MetalPipelineStorage: discarding {} and rebuilding (mismatch: {})",
        title_root_.string(), mismatch);
    ReleaseLock();
    std::filesystem::remove_all(title_root_, ec);
    if (ec) {
      REXLOG_WARN("MetalPipelineStorage: could not clear {}: {}",
             title_root_.string(), ec.message());
      title_root_.clear();
      return false;
    }
    std::filesystem::create_directories(title_root_, ec);
    if (ec || !AcquireLock()) {
      title_root_.clear();
      return false;
    }
    if (!WriteManifest(identity)) {
      ReleaseLock();
      title_root_.clear();
      return false;
    }
  }

  std::filesystem::create_directories(metallib_dir(), ec);

  bytes_on_open_ = DirectorySizeBytes(title_root_);
  bytes_admitted_ = 0;
  admission_stopped_ = false;
  is_open_ = true;

  // 3.6c — enforce the global cap between sessions, now that this title is
  // locked and therefore ineligible.
  EvictToGlobalCapLocked(metal_root);

  REXLOG_INFO(
      "MetalPipelineStorage: opened {} ({} MiB present, per-title cap {} MiB)",
      title_root_.string(), bytes_on_open_ / (1024 * 1024),
      GetMetalSettings().metal_pipeline_cache_title_cap_mib.Get());
  return true;
}

void MetalPipelineStorage::Close() {
  if (is_open_) {
    LogSizeReport("close");
  }
  ReleaseLock();
  is_open_ = false;
  title_root_.clear();
  bytes_on_open_ = 0;
  bytes_admitted_ = 0;
  admission_stopped_ = false;
}

bool MetalPipelineStorage::CanAdmit(uint64_t byte_count) const {
  if (!is_open_ || admission_stopped_) {
    return false;
  }
  const uint64_t cap = GetMetalSettings().metal_pipeline_cache_title_cap_mib.Get() * 1024ull * 1024ull;
  return bytes_on_open_ + bytes_admitted_ + byte_count <= cap;
}

void MetalPipelineStorage::RecordAdmitted(uint64_t byte_count) {
  if (!is_open_) {
    return;
  }
  bytes_admitted_ += byte_count;
  if (byte_count) {
    ++admitted_entry_count_;
  }
  const uint64_t cap = GetMetalSettings().metal_pipeline_cache_title_cap_mib.Get() * 1024ull * 1024ull;
  if (!admission_stopped_ && bytes_on_open_ + bytes_admitted_ >= cap) {
    // 3.6e — stop admitting, keep serving. Deleting live entries or rebuilding
    // would thrash exactly when the title is busiest.
    admission_stopped_ = true;
    REXLOG_WARN(
        "MetalPipelineStorage: title {:08X} reached its {} MiB cap; no new "
        "artifacts will be admitted this session (existing entries still used)",
        title_id_, GetMetalSettings().metal_pipeline_cache_title_cap_mib.Get());
  }
}

void MetalPipelineStorage::LogSizeReport(const char* reason) const {
  if (!is_open_) {
    return;
  }
  const uint64_t cap_mib = GetMetalSettings().metal_pipeline_cache_title_cap_mib.Get();
  const uint64_t metallib_bytes = DirectorySizeBytes(metallib_dir());
  std::error_code ec;
  uint64_t archive_bytes = 0;
  if (std::filesystem::exists(binary_archive_path(), ec) && !ec) {
    archive_bytes = DirectorySizeBytes(binary_archive_path());
  }
  const uint64_t total = metallib_bytes + archive_bytes;
  const double pct_of_cap =
      cap_mib ? 100.0 * double(total) / double(cap_mib * 1024ull * 1024ull) : 0.0;
  // 3.6f — the number that decides whether 512 MiB survives review.
  REXLOG_INFO(
      "MetalPipelineStorage SIZE ({}): title {:08X} M1(metallib)={} KiB, "
      "M2(archive)={} KiB, total={} KiB = {:.1f}% of the {} MiB per-title cap",
      reason ? reason : "", title_id_, metallib_bytes / 1024,
      archive_bytes / 1024, total / 1024, pct_of_cap, cap_mib);
  if (pct_of_cap > 50.0) {
    REXLOG_WARN(
        "MetalPipelineStorage: title {:08X} exceeds 50% of its per-title "
        "allowance — revise the cap default from this evidence rather than "
        "shipping close to the boundary (task 3.6f)",
        title_id_);
  }
}

namespace {
// Per-entry header for an M1 metallib. Deliberately small: the storage-wide
// provenance was validated at Open, so an entry only has to prove it is the
// entry that was asked for and that it is whole.
#pragma pack(push, 1)
struct MetallibFileHeader {
  uint32_t magic;
  uint32_t format_version;
  uint64_t key;
  uint64_t metallib_size;
};
#pragma pack(pop)
constexpr uint32_t kMetallibMagic = 0x314D4C4Du;  // 'MLM1'
// A metallib far larger than this is a corrupt length field, not a shader.
constexpr uint64_t kMaxMetallibBytes = 64ull * 1024ull * 1024ull;
}  // namespace

bool MetalPipelineStorage::LoadMetallib(uint64_t key,
                                        std::vector<uint8_t>* metallib_out) const {
  if (!is_open_ || !metallib_out) {
    return false;
  }
  const std::filesystem::path path =
      metallib_dir() / fmt::format("{:016X}.metallib", key);
  FILE* file = fopen(path.string().c_str(), "rb");
  if (!file) {
    return false;  // Ordinary miss.
  }
  MetallibFileHeader header = {};
  bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
            header.magic == kMetallibMagic &&
            header.format_version == MetalPipelineStorageIdentity::kFormatVersion &&
            header.key == key && header.metallib_size &&
            header.metallib_size <= kMaxMetallibBytes;
  if (ok) {
    metallib_out->resize(header.metallib_size);
    ok = fread(metallib_out->data(), 1, header.metallib_size, file) ==
         header.metallib_size;
  }
  fclose(file);
  if (!ok) {
    // 3.5 — corrupt or truncated is a MISS, never a fault. Remove it so the
    // cold translation that follows can republish a whole one.
    metallib_out->clear();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REXLOG_WARN("MetalPipelineStorage: discarded corrupt metallib entry {:016X}", key);
    return false;
  }
  return true;
}

bool MetalPipelineStorage::StoreMetallib(uint64_t key,
                                         const std::vector<uint8_t>& metallib) {
  if (!is_open_ || metallib.empty() || metallib.size() > kMaxMetallibBytes) {
    return false;
  }
  const uint64_t total = sizeof(MetallibFileHeader) + metallib.size();
  // 3.6e — at cap, stop admitting NEW artifacts; existing ones keep serving.
  if (!CanAdmit(total)) {
    RecordAdmitted(0);  // Latches admission_stopped_ and logs once.
    return false;
  }
  std::vector<uint8_t> buffer(total);
  MetallibFileHeader header = {};
  header.magic = kMetallibMagic;
  header.format_version = MetalPipelineStorageIdentity::kFormatVersion;
  header.key = key;
  header.metallib_size = metallib.size();
  std::memcpy(buffer.data(), &header, sizeof(header));
  std::memcpy(buffer.data() + sizeof(header), metallib.data(), metallib.size());
  if (!WriteFileAtomic(metallib_dir() / fmt::format("{:016X}.metallib", key),
                       buffer.data(), buffer.size())) {
    return false;
  }
  RecordAdmitted(total);
  return true;
}

namespace {
// One record, all components, or nothing (3.10). Sizes are declared up front so
// a short file is detected before any component is trusted.
#pragma pack(push, 1)
struct GeometryFileHeader {
  uint32_t magic;
  uint32_t format_version;
  uint64_t key;
  uint32_t reflection;
  uint32_t function_name_size;
  uint64_t blob_size;
  uint64_t aux_blob_size;
};
#pragma pack(pop)
constexpr uint32_t kGeometryMagic = 0x314D4547u;  // 'GEM1'
constexpr uint32_t kMaxFunctionNameBytes = 4096u;
}  // namespace

bool MetalPipelineStorage::LoadGeometryRecord(uint64_t key,
                                              GeometryRecord* record_out) const {
  if (!is_open_ || !record_out) {
    return false;
  }
  const std::filesystem::path path =
      metallib_dir() / fmt::format("{:016X}.geom", key);
  FILE* file = fopen(path.string().c_str(), "rb");
  if (!file) {
    return false;  // Ordinary miss.
  }
  GeometryFileHeader header = {};
  bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
            header.magic == kGeometryMagic &&
            header.format_version == MetalPipelineStorageIdentity::kFormatVersion &&
            header.key == key && header.reflection != 0 &&
            header.function_name_size &&
            header.function_name_size <= kMaxFunctionNameBytes &&
            header.blob_size && header.blob_size <= kMaxMetallibBytes &&
            header.aux_blob_size <= kMaxMetallibBytes;
  GeometryRecord record;
  if (ok) {
    record.reflection = header.reflection;
    record.function_name.resize(header.function_name_size);
    ok = fread(record.function_name.data(), 1, header.function_name_size, file) ==
         header.function_name_size;
  }
  if (ok) {
    record.blob.resize(header.blob_size);
    ok = fread(record.blob.data(), 1, header.blob_size, file) == header.blob_size;
  }
  if (ok && header.aux_blob_size) {
    record.aux_blob.resize(header.aux_blob_size);
    ok = fread(record.aux_blob.data(), 1, header.aux_blob_size, file) ==
         header.aux_blob_size;
  }
  fclose(file);
  if (!ok) {
    // Any component bad => the WHOLE record is a miss. Never return a
    // partially populated record: half of it would be fresh and half stale,
    // and the pipeline built from that mixture would be silently wrong.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REXLOG_WARN("MetalPipelineStorage: discarded corrupt geometry record {:016X}", key);
    return false;
  }
  *record_out = std::move(record);
  return true;
}

bool MetalPipelineStorage::StoreGeometryRecord(uint64_t key,
                                               const GeometryRecord& record) {
  if (!is_open_ || record.blob.empty() || record.function_name.empty() ||
      !record.reflection || record.blob.size() > kMaxMetallibBytes ||
      record.aux_blob.size() > kMaxMetallibBytes ||
      record.function_name.size() > kMaxFunctionNameBytes) {
    return false;
  }
  const uint64_t total = sizeof(GeometryFileHeader) + record.function_name.size() +
                         record.blob.size() + record.aux_blob.size();
  if (!CanAdmit(total)) {
    RecordAdmitted(0);
    return false;
  }
  GeometryFileHeader header = {};
  header.magic = kGeometryMagic;
  header.format_version = MetalPipelineStorageIdentity::kFormatVersion;
  header.key = key;
  header.reflection = record.reflection;
  header.function_name_size = uint32_t(record.function_name.size());
  header.blob_size = record.blob.size();
  header.aux_blob_size = record.aux_blob.size();

  std::vector<uint8_t> buffer(total);
  size_t offset = 0;
  auto append = [&buffer, &offset](const void* data, size_t size) {
    std::memcpy(buffer.data() + offset, data, size);
    offset += size;
  };
  append(&header, sizeof(header));
  append(record.function_name.data(), record.function_name.size());
  append(record.blob.data(), record.blob.size());
  if (!record.aux_blob.empty()) {
    append(record.aux_blob.data(), record.aux_blob.size());
  }
  // One atomic publish for the whole record, so a crash mid-write cannot leave
  // a record whose components disagree.
  if (!WriteFileAtomic(metallib_dir() / fmt::format("{:016X}.geom", key),
                       buffer.data(), buffer.size())) {
    return false;
  }
  RecordAdmitted(total);
  return true;
}

bool MetalPipelineStorage::WriteFileAtomic(const std::filesystem::path& path,
                                           const void* data, size_t size) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  FILE* file = fopen(tmp.string().c_str(), "wb");
  if (!file) {
    return false;
  }
  bool ok = !size || fwrite(data, 1, size, file) == size;
  if (fflush(file) != 0) {
    ok = false;
  }
  fclose(file);
  std::error_code ec;
  if (!ok) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  // Atomic publish — a crash mid-write cannot leave a torn file at `path`.
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

bool MetalPipelineStorage::LoadAndValidateManifest(
    const MetalPipelineStorageIdentity& identity, std::string* mismatch_out) {
  const std::filesystem::path manifest_path = title_root_ / kManifestName;
  FILE* file = fopen(manifest_path.string().c_str(), "rb");
  if (!file) {
    *mismatch_out = "no manifest (new or previously discarded)";
    return false;
  }
  ManifestFile stored = {};
  const size_t read = fread(&stored, 1, sizeof(stored), file);
  fclose(file);
  // 3.5 — truncated or garbage is treated as absent, never as fatal.
  if (read != sizeof(stored) || stored.magic != kManifestMagic) {
    *mismatch_out = "manifest truncated or corrupt";
    return false;
  }
  if (stored.title_id != title_id_) {
    *mismatch_out = "manifest belongs to a different title";
    return false;
  }
  MetalPipelineStorageIdentity on_disk;
  on_disk.format_version = stored.format_version;
  on_disk.translator_schema = stored.translator_schema;
  on_disk.dxc_artifact_hash = stored.dxc_artifact_hash;
  on_disk.msc_artifact_hash = stored.msc_artifact_hash;
  on_disk.device_hash = stored.device_hash;
  on_disk.os_build_hash = stored.os_build_hash;
  on_disk.converter_options_hash = stored.converter_options_hash;
  if (on_disk != identity) {
    *mismatch_out = identity.DescribeMismatch(on_disk);
    return false;
  }
  // 3.6c — record access once per successful launch, in the manifest. Nothing
  // here depends on filesystem atime.
  return WriteManifest(identity);
}

bool MetalPipelineStorage::WriteManifest(
    const MetalPipelineStorageIdentity& identity) {
  ManifestFile out = {};
  out.magic = kManifestMagic;
  out.format_version = identity.format_version;
  out.translator_schema = identity.translator_schema;
  out.title_id = title_id_;
  out.dxc_artifact_hash = identity.dxc_artifact_hash;
  out.msc_artifact_hash = identity.msc_artifact_hash;
  out.device_hash = identity.device_hash;
  out.os_build_hash = identity.os_build_hash;
  out.converter_options_hash = identity.converter_options_hash;
  out.last_access_seconds = NowSeconds();
  return WriteFileAtomic(title_root_ / kManifestName, &out, sizeof(out));
}

void MetalPipelineStorage::EvictToGlobalCapLocked(
    const std::filesystem::path& metal_root) {
  const uint64_t global_cap =
      GetMetalSettings().metal_pipeline_cache_global_cap_mib.Get() * 1024ull * 1024ull;
  if (!global_cap) {
    return;
  }

  struct Candidate {
    std::filesystem::path path;
    uint64_t bytes;
    int64_t last_access;
  };
  std::vector<Candidate> candidates;
  uint64_t total = 0;

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(metal_root, ec)) {
    if (ec) {
      return;
    }
    std::error_code dir_ec;
    if (!entry.is_directory(dir_ec) || dir_ec) {
      continue;
    }
    const uint64_t bytes = DirectorySizeBytes(entry.path());
    total += bytes;
    // 3.6d — never the active title.
    if (entry.path() == title_root_) {
      continue;
    }
    // NEVER the DXIL artifact store. It lives under metal/ but is NOT a title
    // directory: it has no title manifest, so it would read as
    // last_access = 0 and sort FIRST for eviction — the newest and most
    // broadly useful store deleted before any stale title. Its bytes still
    // count toward `total` above, because it does consume the global cap; it
    // enforces its own bound internally
    // (metal_dxil_store.h, cvar ::rex::graphics::metal::GetMetalSettings().metal_dxil_cache_cap_mib.Get()).
    if (entry.path().filename() == kMetalDxilStoreDirName) {
      continue;
    }
    // 3.6d — never a directory another process holds. flock is advisory, so
    // this only sees peers that took it, which is exactly our own instances.
    const std::filesystem::path peer_lock = entry.path() / kLockName;
    int fd = open(peer_lock.string().c_str(), O_RDONLY);
    if (fd >= 0) {
      const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
      if (!held) {
        flock(fd, LOCK_UN);
      }
      close(fd);
      if (held) {
        continue;
      }
    }
    int64_t last_access = 0;
    FILE* manifest = fopen((entry.path() / kManifestName).string().c_str(), "rb");
    if (manifest) {
      ManifestFile stored = {};
      if (fread(&stored, 1, sizeof(stored), manifest) == sizeof(stored) &&
          stored.magic == kManifestMagic) {
        last_access = stored.last_access_seconds;
      }
      fclose(manifest);
    }
    candidates.push_back({entry.path(), bytes, last_access});
  }

  if (total <= global_cap) {
    return;
  }

  // Least-recently-used whole directories first. Entry-level LRU is
  // deliberately out of scope for the first implementation (3.6g): whole-
  // directory eviction is bounded and recoverable without it.
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.last_access < b.last_access;
            });

  for (const Candidate& candidate : candidates) {
    if (total <= global_cap) {
      break;
    }
    std::error_code remove_ec;
    std::filesystem::remove_all(candidate.path, remove_ec);
    if (remove_ec) {
      continue;
    }
    total -= std::min(total, candidate.bytes);
    REXLOG_INFO(
        "MetalPipelineStorage: evicted {} ({} MiB) to stay under the {} MiB "
        "global cap",
        candidate.path.filename().string(), candidate.bytes / (1024 * 1024),
        GetMetalSettings().metal_pipeline_cache_global_cap_mib.Get());
  }
}

bool MetalPipelineStorage::AcquireLock() {
  lock_path_ = title_root_ / kLockName;
  lock_fd_ = open(lock_path_.string().c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd_ < 0) {
    return false;
  }
  if (flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    close(lock_fd_);
    lock_fd_ = -1;
    return false;
  }
  return true;
}

void MetalPipelineStorage::ReleaseLock() {
  if (lock_fd_ >= 0) {
    flock(lock_fd_, LOCK_UN);
    close(lock_fd_);
    lock_fd_ = -1;
  }
  lock_path_.clear();
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
