/**
 * @file        runtime/embedded_metadata.cpp
 * @brief       Embedded metadata registry implementation.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/embedded_metadata.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rex {
namespace {

struct RegisteredEmbeddedMetadataAsset {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

std::string NormalizeMetadataPath(const std::filesystem::path& relative_path) {
  std::string normalized = relative_path.generic_string();
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  return normalized;
}

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, RegisteredEmbeddedMetadataAsset>& Registry() {
  static std::unordered_map<std::string, RegisteredEmbeddedMetadataAsset> registry;
  return registry;
}

}  // namespace

bool RegisterEmbeddedMetadataAsset(std::string_view relative_path, const std::uint8_t* data,
                                   std::size_t size) {
  if (relative_path.empty() || !data || size == 0) {
    return false;
  }

  const std::string normalized = NormalizeMetadataPath(std::filesystem::path(relative_path));
  if (normalized.empty()) {
    return false;
  }

  std::lock_guard lock(RegistryMutex());
  Registry()[normalized] = RegisteredEmbeddedMetadataAsset{data, size};
  return true;
}

std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadataAsset(
    const std::filesystem::path& relative_path) {
  const std::string normalized = NormalizeMetadataPath(relative_path);
  if (normalized.empty()) {
    return std::nullopt;
  }

  std::lock_guard lock(RegistryMutex());
  auto it = Registry().find(normalized);
  if (it == Registry().end()) {
    return std::nullopt;
  }

  return EmbeddedMetadataAsset{
      std::span<const std::uint8_t>(it->second.data, it->second.size),
  };
}

}  // namespace rex
