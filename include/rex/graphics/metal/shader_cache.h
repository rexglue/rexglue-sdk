/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Static Recompilation Runtime
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 *
 * Metal shader disk cache.
 *
 * Caches compiled metallib binaries to disk so that the expensive
 * DXBC → DXIL → Metal IR pipeline only runs once per unique shader.
 * Subsequent loads read the pre-compiled metallib directly.
 *
 * Cache key: hash of (ucode_hash, modification, shader_stage).
 * File format: simple header + function name + metallib blob.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::graphics::metal {

class MetalShaderCache {
 public:
  struct CachedMetallib {
    std::string function_name;
    std::vector<uint8_t> metallib_data;
  };

  struct CacheStats {
    size_t entry_count = 0;
    size_t total_bytes = 0;
  };

  MetalShaderCache() = default;
  ~MetalShaderCache() = default;

  // Initialize the cache with a directory for disk storage.
  void Initialize(const std::filesystem::path& cache_dir);
  void Shutdown();

  bool IsInitialized() const { return initialized_; }
  const std::filesystem::path& cache_dir() const { return cache_dir_; }

  // Compute a cache key from shader identity.
  static uint64_t GetCacheKey(uint64_t ucode_hash, uint64_t modification,
                              uint32_t stage);

  // Load a cached metallib. Checks memory first, then disk.
  bool Load(uint64_t cache_key, CachedMetallib* out);

  // Store a compiled metallib to memory and disk.
  void Store(uint64_t cache_key, std::string_view function_name,
             const uint8_t* metallib_data, size_t metallib_size);

  CacheStats GetStats() const;

 private:
  struct MemoryEntry {
    std::string function_name;
    std::vector<uint8_t> metallib_data;
  };

  void ShutdownLocked();  // mutex_ must be held
  bool LoadFromDisk(uint64_t cache_key, CachedMetallib* out);
  bool StoreToDisk(uint64_t cache_key, const CachedMetallib& in);
  std::filesystem::path GetDiskPath(uint64_t cache_key) const;

  mutable std::mutex mutex_;
  std::atomic<bool> initialized_ = false;
  std::filesystem::path cache_dir_;
  std::unordered_map<uint64_t, MemoryEntry> cache_;
};

}  // namespace rex::graphics::metal
