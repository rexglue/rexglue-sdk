/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_dxil_store.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "fmt/format.h"
#include "xxhash.h"
#include <rex/logging/log_category.h>
#include "rex/graphics/metal/metal_converter_options.h"
#include "rex/graphics/metal/metal_pipeline_storage.h"

// PROMOTED DEFAULT-ON 2026-08-05 on the task 2.4 evidence.
//
// WHAT THIS IS: a RESILIENT INTERMEDIATE cache. It is not a warm-start cache
// and must never be described as one — on an ordinary warm launch it does
// nothing at all, because the metallib store hits first and returns before
// DXBC->DXIL runs (measured: zero DXIL activity, S1).
//
// What it does do, both measured:
//   1. SURVIVES METALLIB-INVALIDATING CHANGES. A metallib is compiled against
//      a GPU family and an OS build by the Metal Shader Converter; DXIL is
//      none of those. When an OS update, an MSC upgrade, a different GPU or a
//      shader-ABI bump legitimately discards every metallib, the dxilconv work
//      does not have to be re-paid. Measured on the title-invalidated arm:
//      Sonic 295.2 ms -> 0.0 ms (100 hits), worst frame 242.7 -> 150.2 ms;
//      JC2 584.3 ms -> 0.0 ms (216 hits), worst frame 1015.5 -> 611.9 ms.
//      MSC time was unchanged in both, which is what confirms the attribution.
//   2. DEDUPLICATES IDENTICAL DXBC WITHIN A COLD RUN. Distinct
//      (ucode, modification) pairs can emit byte-identical DXBC, which the
//      metallib key cannot express: 11 of Sonic's 100 conversions and 7 of
//      JC2's 216 were duplicates.
//
// Cold cost is a lookup plus a write (Sonic 1.4-7.4 ms + 16.1-16.3 ms; JC2
// 6.2 + 38.6 ms). The cold arms measured slightly FASTER with the gate on, but
// that difference is WITHIN RUN-TO-RUN NOISE and is not claimed as a gain — the
// defensible statement is no measurable cold regression.
//
// Setting it false is the EXACT rollback: no DXIL reads, no writes, no store
// opened. Both arms log their effective state (grep MetalDxilGate).
// Config migration for the promotion. Resets ONLY configs still carrying the
// old default (false); anyone who deliberately set a value keeps it. A
// deliberate pre-promotion opt-out that happens to equal the old default is
// indistinguishable from the default itself -- the config file records values,
// not provenance -- so it is reset once. The flag shipped default-off for a
// single day (2026-08-04 to 2026-08-05), so that population is test configs.
// Every opt-out made after this point is permanent.

namespace rex {
namespace graphics {
namespace metal {

namespace {

constexpr char kManifestName[] = "identity.manifest";
constexpr char kLockName[] = ".lock";
constexpr uint32_t kManifestMagic = 0x4C58445Bu;   // '[DXL'
constexpr uint32_t kRecordMagic = 0x5258445Bu;     // '[DXR'
// A DXIL module for one guest shader is single-digit KiB (2748-3452 B measured
// across the task 2.1 corpus). 64 MiB is far above anything legitimate and
// exists so a corrupt size field cannot drive an enormous allocation.
constexpr uint64_t kMaxDxilBytes = 64ull * 1024ull * 1024ull;

#pragma pack(push, 1)
struct ManifestFile {
  uint32_t magic;
  uint32_t format_version;
  uint64_t dxc_artifact_hash;
  uint64_t converter_options_hash;
  uint32_t dxilconv_generation;
  uint32_t reserved;
};

// Every record carries its own full key. The filename is a 64-bit hash, so the
// echo is what turns a filename collision into a miss instead of a well-formed
// artifact of a different shader.
struct RecordHeader {
  uint32_t magic;
  uint32_t format_version;
  uint64_t key_words[MetalDxilArtifactKey::kWordCount];
  uint64_t payload_size;
  uint64_t payload_hash;
};
#pragma pack(pop)

uint64_t DirectorySizeBytes(const std::filesystem::path& path) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
    if (ec) {
      break;
    }
    std::error_code file_ec;
    if (entry.is_regular_file(file_ec) && !file_ec) {
      total += entry.file_size(file_ec);
    }
  }
  return total;
}

}  // namespace

bool MetalDxilStoreIdentity::operator==(
    const MetalDxilStoreIdentity& other) const {
  return format_version == other.format_version &&
         dxc_artifact_hash == other.dxc_artifact_hash &&
         converter_options_hash == other.converter_options_hash &&
         dxilconv_generation == other.dxilconv_generation;
}

std::string MetalDxilStoreIdentity::DescribeMismatch(
    const MetalDxilStoreIdentity& other) const {
  std::string out;
  auto add = [&out](const char* name) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  };
  if (format_version != other.format_version) add("format version");
  if (dxc_artifact_hash != other.dxc_artifact_hash) add("libdxilconv");
  if (converter_options_hash != other.converter_options_hash) {
    add("dxilconv options");
  }
  if (dxilconv_generation != other.dxilconv_generation) {
    add("dxilconv generation");
  }
  return out.empty() ? "none" : out;
}

MetalDxilStoreIdentity ComputeMetalDxilStoreIdentity() {
  MetalDxilStoreIdentity identity;
  // Reuses the pipeline storage's computation so the two cannot disagree about
  // which libdxilconv is loaded or what options this process passes. Only the
  // DXIL-relevant fields are taken; the MSC hash, device and OS build are
  // deliberately dropped — carrying them is precisely the coupling that would
  // make this store worthless.
  const MetalPipelineStorageIdentity full =
      ComputeMetalPipelineStorageIdentity(nullptr);
  identity.dxc_artifact_hash = full.dxc_artifact_hash;
  identity.converter_options_hash = full.converter_options_hash;
  identity.dxilconv_generation = MetalTranslatorConfig::kDxilconvGeneration;
  return identity;
}

MetalDxilArtifactKey MetalDxilArtifactKey::ForDxbc(
    const std::vector<uint8_t>& dxbc) {
  MetalDxilArtifactKey key;
  key.dxbc_size = dxbc.size();
  const XXH128_hash_t hash =
      XXH3_128bits(dxbc.empty() ? nullptr : dxbc.data(), dxbc.size());
  key.dxbc_hash_low = hash.low64;
  key.dxbc_hash_high = hash.high64;
  // Same prefixed hash the storage identity computes, so the coarse gate (the
  // manifest) and the fine gate (this key) cannot disagree about what the
  // option string was.
  const std::string prefixed =
      "dxilconv-options:" + MetalDxbcToDxilEffectiveOptions();
  key.converter_options_hash = XXH3_64bits(prefixed.data(), prefixed.size());
  key.dxilconv_generation = MetalTranslatorConfig::kDxilconvGeneration;
  return key;
}

void MetalDxilArtifactKey::SerializeWords(
    uint64_t (&out)[kWordCount]) const {
  out[0] = key_version;
  out[1] = dxbc_size;
  out[2] = dxbc_hash_low;
  out[3] = dxbc_hash_high;
  out[4] = converter_options_hash;
  out[5] = dxilconv_generation;
}

uint64_t MetalDxilArtifactKey::FileKey() const {
  uint64_t words[kWordCount];
  SerializeWords(words);
  return XXH3_64bits(words, sizeof(words));
}

bool MetalDxilArtifactKey::operator==(const MetalDxilArtifactKey& other) const {
  uint64_t a[kWordCount];
  uint64_t b[kWordCount];
  SerializeWords(a);
  other.SerializeWords(b);
  return std::memcmp(a, b, sizeof(a)) == 0;
}

const char* MetalDxilLoadResultName(MetalDxilLoadResult result) {
  switch (result) {
    case MetalDxilLoadResult::kHit:
      return "hit";
    case MetalDxilLoadResult::kMissAbsent:
      return "miss:absent";
    case MetalDxilLoadResult::kMissKeyMismatch:
      return "miss:key";
    case MetalDxilLoadResult::kMissUnreadable:
      return "miss:unreadable";
    case MetalDxilLoadResult::kRejectedCorrupt:
      return "rejected:corrupt";
  }
  return "miss:unknown";
}

MetalDxilStore::~MetalDxilStore() { Close(); }

bool MetalDxilStore::Open(const std::filesystem::path& cache_root,
                          const MetalDxilStoreIdentity& identity) {
  Close();
  if (cache_root.empty()) {
    return false;
  }
  root_ = cache_root / "metal" / kMetalDxilStoreDirName;

  std::error_code ec;
  std::filesystem::create_directories(root_, ec);
  if (ec) {
    REXLOG_WARN("MetalDxilStore: cannot create {}: {} — no DXIL store",
           root_.string(), ec.message());
    root_.clear();
    return false;
  }

  if (!AcquireLock()) {
    // Another instance holds it. Fail-soft and identical in shape to the
    // per-title store: no DXIL caching this session rather than two writers.
    REXLOG_WARN("MetalDxilStore: {} is held by another process — no DXIL store",
           root_.string());
    root_.clear();
    return false;
  }

  std::string mismatch;
  if (!LoadAndValidateManifest(identity, &mismatch)) {
    // Whole-directory discard. Everything here was produced under a different
    // converter contract, and no per-entry key can see that.
    REXLOG_INFO("MetalDxilStore: discarding {} and rebuilding (mismatch: {})",
           root_.string(), mismatch);
    ReleaseLock();
    std::filesystem::remove_all(root_, ec);
    if (ec) {
      REXLOG_WARN("MetalDxilStore: could not clear {}: {}", root_.string(),
             ec.message());
      root_.clear();
      return false;
    }
    std::filesystem::create_directories(root_, ec);
    if (ec || !AcquireLock()) {
      root_.clear();
      return false;
    }
    if (!WriteManifest(identity)) {
      ReleaseLock();
      root_.clear();
      return false;
    }
  }

  // Reclaim before measuring, so bytes_on_open_ reflects the post-eviction
  // size and the session's admission budget is the real remaining headroom.
  evicted_entries_ = 0;
  evicted_bytes_ = 0;
  EvictToCapLocked();
  bytes_on_open_ = DirectorySizeBytes(root_);
  bytes_admitted_ = 0;
  admission_stopped_ = false;
  hits_ = misses_ = corrupt_rejects_ = stores_ = 0;
  is_open_ = true;
  REXLOG_INFO(
      "MetalDxilStore: opened {} ({} KiB present, cap {} MiB, evicted {} "
      "entries / {} KiB)",
      root_.string(), bytes_on_open_ / 1024, GetMetalSettings().metal_dxil_cache_cap_mib.Get(),
      evicted_entries_, evicted_bytes_ / 1024);
  return true;
}

void MetalDxilStore::Close() {
  if (is_open_) {
    REXLOG_INFO(
        "MetalDxilStore: closing — hits={} misses={} corrupt_rejects={} "
        "stores={} admitted={} KiB",
        hits_, misses_, corrupt_rejects_, stores_, bytes_admitted_ / 1024);
  }
  ReleaseLock();
  is_open_ = false;
  root_.clear();
}

bool MetalDxilStore::AcquireLock() {
  lock_path_ = root_ / kLockName;
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

void MetalDxilStore::ReleaseLock() {
  if (lock_fd_ >= 0) {
    flock(lock_fd_, LOCK_UN);
    close(lock_fd_);
    lock_fd_ = -1;
  }
}

void MetalDxilStore::EvictToCapLocked() {
  const uint64_t cap = GetMetalSettings().metal_dxil_cache_cap_mib.Get() * 1024ull * 1024ull;
  if (!cap) {
    return;
  }
  struct Entry {
    std::filesystem::path path;
    uint64_t bytes;
    int64_t mtime;
  };
  std::vector<Entry> entries;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(root_, ec)) {
    if (ec) {
      return;
    }
    std::error_code file_ec;
    if (!item.is_regular_file(file_ec) || file_ec) {
      continue;
    }
    if (item.path().extension() != ".dxil") {
      continue;  // never the manifest or the lock
    }
    struct stat st = {};
    if (stat(item.path().string().c_str(), &st) != 0) {
      continue;
    }
    const uint64_t bytes = uint64_t(st.st_size);
    total += bytes;
    entries.push_back({item.path(), bytes, st.st_mtimespec.tv_sec});
  }
  // Reclaim to a LOW-WATER MARK whenever the store is above it — NOT only when
  // it is over the cap. Over-cap is a state that can never occur: Store()
  // refuses admission at the ceiling, so a full store would sit exactly at the
  // cap forever, evict nothing at Open, and admit nothing for the rest of its
  // life. Reclaiming to 75% guarantees every session real headroom, which is
  // what makes this an LRU cache that rotates rather than a bucket that fills
  // once. (Found by the conformance case: with an over-cap trigger it evicted
  // 0 entries and CanAdmit was false — bounded, and useless.)
  const uint64_t target = cap - cap / 4;
  if (total <= target) {
    return;
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
  for (const Entry& entry : entries) {
    if (total <= target) {
      break;
    }
    std::error_code remove_ec;
    if (std::filesystem::remove(entry.path, remove_ec) && !remove_ec) {
      total -= entry.bytes;
      ++evicted_entries_;
      evicted_bytes_ += entry.bytes;
    }
  }
  if (evicted_entries_) {
    REXLOG_INFO(
        "MetalDxilStore: evicted {} least-recently-used artifacts ({} KiB) to "
        "stay under the {} MiB cap",
        evicted_entries_, evicted_bytes_ / 1024,
        GetMetalSettings().metal_dxil_cache_cap_mib.Get());
  }
}

bool MetalDxilStore::LoadAndValidateManifest(
    const MetalDxilStoreIdentity& identity, std::string* mismatch_out) {
  FILE* file = fopen((root_ / kManifestName).string().c_str(), "rb");
  if (!file) {
    *mismatch_out = "no manifest (new or previously discarded)";
    return false;
  }
  ManifestFile stored = {};
  const size_t read = fread(&stored, 1, sizeof(stored), file);
  fclose(file);
  if (read != sizeof(stored) || stored.magic != kManifestMagic) {
    *mismatch_out = "manifest truncated or corrupt";
    return false;
  }
  MetalDxilStoreIdentity on_disk;
  on_disk.format_version = stored.format_version;
  on_disk.dxc_artifact_hash = stored.dxc_artifact_hash;
  on_disk.converter_options_hash = stored.converter_options_hash;
  on_disk.dxilconv_generation = stored.dxilconv_generation;
  if (on_disk != identity) {
    *mismatch_out = identity.DescribeMismatch(on_disk);
    return false;
  }
  return true;
}

bool MetalDxilStore::WriteManifest(const MetalDxilStoreIdentity& identity) {
  ManifestFile out = {};
  out.magic = kManifestMagic;
  out.format_version = identity.format_version;
  out.dxc_artifact_hash = identity.dxc_artifact_hash;
  out.converter_options_hash = identity.converter_options_hash;
  out.dxilconv_generation = identity.dxilconv_generation;
  return MetalPipelineStorage::WriteFileAtomic(root_ / kManifestName, &out,
                                               sizeof(out));
}

std::filesystem::path MetalDxilStore::PathForKey(
    const MetalDxilArtifactKey& key) const {
  return root_ / fmt::format("{:016X}.dxil", key.FileKey());
}

bool MetalDxilStore::CanAdmit(uint64_t byte_count) const {
  const uint64_t cap = GetMetalSettings().metal_dxil_cache_cap_mib.Get() * 1024ull * 1024ull;
  if (!cap) {
    return true;
  }
  return bytes_on_open_ + bytes_admitted_ + byte_count <= cap;
}

MetalDxilLoadResult MetalDxilStore::Load(const MetalDxilArtifactKey& key,
                                         std::vector<uint8_t>* dxil_out) {
  if (!is_open_ || !dxil_out) {
    return MetalDxilLoadResult::kMissAbsent;
  }
  const std::filesystem::path path = PathForKey(key);
  FILE* file = fopen(path.string().c_str(), "rb");
  if (!file) {
    ++misses_;
    return MetalDxilLoadResult::kMissAbsent;
  }
  RecordHeader header = {};
  if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
      header.magic != kRecordMagic ||
      header.format_version != MetalDxilStoreIdentity::kFormatVersion ||
      header.payload_size == 0 || header.payload_size > kMaxDxilBytes) {
    fclose(file);
    ++misses_;
    return MetalDxilLoadResult::kMissUnreadable;
  }

  // KEY ECHO. The filename is a 64-bit hash; this is what makes a collision a
  // miss rather than a perfectly well-formed artifact of a different shader.
  uint64_t expected[MetalDxilArtifactKey::kWordCount];
  key.SerializeWords(expected);
  if (std::memcmp(header.key_words, expected, sizeof(expected)) != 0) {
    fclose(file);
    ++misses_;
    return MetalDxilLoadResult::kMissKeyMismatch;
  }

  // Parenthesised size_t(...) here would be a function declaration (most
  // vexing parse); the braced form is unambiguous.
  const size_t payload_size = size_t(header.payload_size);
  std::vector<uint8_t> payload(payload_size);
  const size_t read = fread(payload.data(), 1, payload.size(), file);
  fclose(file);
  if (read != payload.size()) {
    ++misses_;
    return MetalDxilLoadResult::kMissUnreadable;
  }

  if (XXH3_64bits(payload.data(), payload.size()) != header.payload_hash) {
    // NOT a miss. The key matched and the record is well-formed, but the bytes
    // are not the bytes that were written: this entry is corrupt. Reject it,
    // report it, and delete it so the next lookup rebuilds rather than
    // repeating the rejection every launch.
    ++corrupt_rejects_;
    REXLOG_WARN(
        "MetalDxilStore: CORRUPT artifact {} (payload hash mismatch, {} bytes) "
        "— rejected and deleted; it will be rebuilt",
        path.filename().string(), payload.size());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return MetalDxilLoadResult::kRejectedCorrupt;
  }

  // Touch on hit so mtime means "last used", which is what makes the LRU
  // eviction above evict the unused rather than merely the old. Failure is
  // ignored: a store that cannot be touched still serves correctly, it just
  // evicts less well.
  const struct timespec times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
  utimensat(AT_FDCWD, path.string().c_str(), times, 0);

  *dxil_out = std::move(payload);
  ++hits_;
  return MetalDxilLoadResult::kHit;
}

void MetalDxilStore::Forget(const MetalDxilArtifactKey& key) {
  if (!is_open_) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(PathForKey(key), ec);
}

bool MetalDxilStore::Store(const MetalDxilArtifactKey& key,
                           const std::vector<uint8_t>& dxil) {
  if (!is_open_ || dxil.empty() || dxil.size() > kMaxDxilBytes) {
    return false;
  }
  const uint64_t record_bytes = sizeof(RecordHeader) + dxil.size();
  if (!CanAdmit(record_bytes)) {
    if (!admission_stopped_) {
      admission_stopped_ = true;
      REXLOG_INFO(
          "MetalDxilStore: reached the {} MiB cap; no new artifacts this "
          "session (existing ones keep being served)",
          GetMetalSettings().metal_dxil_cache_cap_mib.Get());
    }
    return false;
  }

  RecordHeader header = {};
  header.magic = kRecordMagic;
  header.format_version = MetalDxilStoreIdentity::kFormatVersion;
  key.SerializeWords(header.key_words);
  header.payload_size = dxil.size();
  header.payload_hash = XXH3_64bits(dxil.data(), dxil.size());

  const size_t record_size = size_t(record_bytes);
  std::vector<uint8_t> record(record_size);
  std::memcpy(record.data(), &header, sizeof(header));
  std::memcpy(record.data() + sizeof(header), dxil.data(), dxil.size());

  // Atomic publish (tmp + rename), so a crash mid-write cannot leave a torn
  // file at the real path for a later run to read as a hit.
  if (!MetalPipelineStorage::WriteFileAtomic(PathForKey(key), record.data(),
                                             record.size())) {
    return false;
  }
  bytes_admitted_ += record_bytes;
  ++stores_;
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
