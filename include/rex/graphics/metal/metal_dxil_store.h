/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_DXIL_STORE_H_
#define XENIA_GPU_METAL_METAL_DXIL_STORE_H_

// Durable store for the DXBC->DXIL intermediate
// (adopt-xenios-shader-artifact-caching task 2.2).
//
// WHY THIS IS A SEPARATE STORE AND NOT A KIND IN MetalPipelineStorage.
// Task 1.1 measured that the default-on metallib store already removes 100% of
// both DXBC->DXIL and DXIL->metallib on a warm launch — a metallib hit returns
// before stage 2 runs. A DXIL record living under the per-title manifest would
// therefore never be read: every case that would consult it is a case the
// metallib already served, and every invalidation that discards the metallib
// discards it too.
//
// The asymmetry that makes this store worth having is what the two artifacts
// actually depend on:
//
//   a metallib  depends on the GPU family and the OS build (it is compiled
//               against them) AND on the Metal Shader Converter AND on what our
//               translator emits.
//   DXIL        depends on NONE of those. It is a function of the DXBC bytes,
//               the dxilconv option string, our converter-wrapper generation,
//               and which libdxilconv is loaded.
//
// So on an OS update or an MSC upgrade the metallib must be rebuilt and the
// DXIL must not be. Under one shared manifest that distinction is unavailable,
// and the measured dxilconv work — 292.2 ms (Sonic), 26.3 ms (SF4), 582.7 ms
// (JC2) — is re-paid for nothing. This store's identity contains exactly the
// four things DXIL depends on and nothing else, which is the entire point.
//
// It therefore lives at <cache_root>/metal/dxil/, OUTSIDE any title directory:
// a title directory is discarded wholesale when its manifest mismatches, and
// being inside one would reintroduce the coupling this store exists to break.
// Content-keyed and title-independent, so two titles sharing a DXBC payload
// share the artifact.
//
// FOUR OUTCOMES, DELIBERATELY DISTINCT (they are not shades of "miss"):
//
//   store-manifest mismatch  the whole DXIL directory is discarded at Open.
//                            Everything in it was produced by a different
//                            converter contract.
//   artifact-key mismatch    an ordinary miss. Costs one cold conversion.
//                            Includes the key ECHO check, so a 64-bit filename
//                            collision cannot serve a well-formed artifact of a
//                            different shader (mechanism from XeniOS
//                            metal_shader_cache.cc:579-585).
//   payload corruption       NOT a miss: a stored artifact that is wrong.
//                            Rejected, reported, and the entry is DELETED so
//                            the next lookup rebuilds it. Counted separately —
//                            it must stay zero, and folding it into misses
//                            would hide it inside a number a cold cache also
//                            increments.
//   unverifiable             a miss (see metal_artifact_hit_verify.h): "could
//                            not check" must never resolve to "checked out".

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/graphics/metal/metal_settings.h>

namespace rex {
namespace graphics {
namespace metal {

// The directory name under <cache_root>/metal/. Shared with
// MetalPipelineStorage's global-cap eviction, which must never treat it as a
// title directory: it has no title manifest, so it would sort as
// least-recently-used and be the FIRST thing deleted when the global cap is
// reached.
inline constexpr char kMetalDxilStoreDirName[] = "dxil";

// Everything that changes the DXIL produced for byte-identical DXBC, and
// nothing else. Deliberately does NOT contain the MSC hash, the device, the OS
// build, or the translator schema — a DXIL artifact survives all four, which is
// this store's whole reason to exist.
struct MetalDxilStoreIdentity {
  static constexpr uint32_t kFormatVersion = 1;

  uint32_t format_version = kFormatVersion;
  // Which libdxilconv produced it (size + mtime hash, same helper the pipeline
  // storage identity uses — a version string cannot distinguish our locally
  // patched build from a released one).
  uint64_t dxc_artifact_hash = 0;
  // How dxilconv was invoked (metal_converter_options.h). Measured
  // output-affecting: 2748 B vs 2944 B of DXIL from the same DXBC.
  uint64_t converter_options_hash = 0;
  // Our wrapper generation (MetalTranslatorConfig::kDxilconvGeneration).
  // Generation 1 = fresh-IDxbcConverter-per-Convert; generation 0 artifacts
  // may carry converter call-history corruption that no content hash can see.
  uint32_t dxilconv_generation = 0;

  bool operator==(const MetalDxilStoreIdentity& other) const;
  bool operator!=(const MetalDxilStoreIdentity& other) const {
    return !(*this == other);
  }
  std::string DescribeMismatch(const MetalDxilStoreIdentity& other) const;
};

MetalDxilStoreIdentity ComputeMetalDxilStoreIdentity();

// Per-artifact identity. The option hash and generation appear here AND in the
// store identity above on purpose: the manifest is the coarse gate that
// discards the directory, the key is the fine gate that misses one entry. Port
// note "both, not either" — XeniOS puts them only in the key, so a converter
// change there silently misses every entry and leaves the stale bytes on disk
// forever.
struct MetalDxilArtifactKey {
  static constexpr size_t kWordCount = 6;

  uint32_t key_version = 1;
  uint64_t dxbc_size = 0;
  uint64_t dxbc_hash_low = 0;
  uint64_t dxbc_hash_high = 0;
  uint64_t converter_options_hash = 0;
  uint32_t dxilconv_generation = 0;

  // Full 128-bit content hash of the DXBC plus its size. The size is redundant
  // against a good hash and kept anyway: it is free and it makes a truncated
  // input impossible to alias onto a complete one.
  static MetalDxilArtifactKey ForDxbc(const std::vector<uint8_t>& dxbc);

  void SerializeWords(uint64_t (&out)[kWordCount]) const;
  // Filename identity. 64 bits, so collisions are conceivable; the echo check
  // on load is what makes a collision a miss rather than a wrong artifact.
  uint64_t FileKey() const;
  bool operator==(const MetalDxilArtifactKey& other) const;
};

enum class MetalDxilLoadResult {
  kHit,
  // Ordinary misses. One cold conversion each.
  kMissAbsent,
  kMissKeyMismatch,     // key echo rejected it (collision or stale layout)
  kMissUnreadable,      // short, truncated, or malformed header
  // NOT a miss: a stored artifact whose payload does not match its recorded
  // hash. The entry is deleted so the next lookup rebuilds it.
  kRejectedCorrupt,
};

const char* MetalDxilLoadResultName(MetalDxilLoadResult result);

class MetalDxilStore {
 public:
  MetalDxilStore() = default;
  ~MetalDxilStore();

  MetalDxilStore(const MetalDxilStore&) = delete;
  MetalDxilStore& operator=(const MetalDxilStore&) = delete;

  // Opens (or creates) <cache_root>/metal/dxil/ and validates its manifest.
  // On any mismatch, corruption or truncation the directory is discarded and
  // recreated empty. Fail-soft: false simply means no DXIL store this session.
  bool Open(const std::filesystem::path& cache_root,
            const MetalDxilStoreIdentity& identity);
  void Close();
  bool is_open() const { return is_open_; }
  const std::filesystem::path& root() const { return root_; }

  MetalDxilLoadResult Load(const MetalDxilArtifactKey& key,
                           std::vector<uint8_t>* dxil_out);
  bool Store(const MetalDxilArtifactKey& key, const std::vector<uint8_t>& dxil);

  // Drops one entry. Used when sampled verification rejects a hit: the artifact
  // was served, proved not to be what this build produces, and must not be
  // served again. Deleting rather than merely refusing it in memory is what
  // makes the next launch rebuild instead of repeating the rejection.
  void Forget(const MetalDxilArtifactKey& key);

  // Admission bound. Its own cap because this directory is outside the
  // per-title accounting; on reaching it no NEW artifacts are admitted for the
  // session and existing ones keep being served.
  bool CanAdmit(uint64_t byte_count) const;

  uint64_t evicted_entries() const { return evicted_entries_; }
  uint64_t evicted_bytes() const { return evicted_bytes_; }

  uint64_t hits() const { return hits_; }
  uint64_t misses() const { return misses_; }
  uint64_t corrupt_rejects() const { return corrupt_rejects_; }
  uint64_t stores() const { return stores_; }
  uint64_t bytes_on_open() const { return bytes_on_open_; }
  uint64_t bytes_admitted() const { return bytes_admitted_; }
  bool admission_stopped() const { return admission_stopped_; }

 private:
  bool LoadAndValidateManifest(const MetalDxilStoreIdentity& identity,
                               std::string* mismatch_out);
  bool WriteManifest(const MetalDxilStoreIdentity& identity);
  bool AcquireLock();
  void ReleaseLock();
  std::filesystem::path PathForKey(const MetalDxilArtifactKey& key) const;
  // Reclamation, run once at Open while the store is locked.
  //
  // WHY THIS STORE NEEDS IT AND THE PER-TITLE ONE DOES NOT. MetalPipelineStorage
  // is DISCARDED WHOLESALE whenever its identity changes, so it never
  // accumulates entries from superseded contracts and "stop admitting at the
  // cap" is a complete policy there. This store's entire value is that it is
  // NOT discarded when the metallib identity moves — but a translator change
  // that alters emitted DXBC mints new keys and orphans every old entry
  // permanently. With a ceiling alone the store would fill with artifacts no
  // build will ever ask for again and then stop caching while still occupying
  // its full cap: bounded, and useless. LRU by mtime, touched on every hit.
  //
  // Eviction happens only at Open, never mid-session — deleting live entries
  // while a title is busy would thrash exactly when it is least affordable,
  // which is the same rule MetalPipelineStorage's global cap follows.
  void EvictToCapLocked();

  bool is_open_ = false;
  std::filesystem::path root_;
  std::filesystem::path lock_path_;
  int lock_fd_ = -1;
  uint64_t bytes_on_open_ = 0;
  uint64_t bytes_admitted_ = 0;
  bool admission_stopped_ = false;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t corrupt_rejects_ = 0;
  uint64_t stores_ = 0;
  uint64_t evicted_entries_ = 0;
  uint64_t evicted_bytes_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_DXIL_STORE_H_
