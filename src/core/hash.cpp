/**
 * @file        core/hash.cpp
 * @brief       XXH3 digest helpers for build fingerprints and cache keys
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/hash.h>

#include <fstream>
#include <memory>

#include <fmt/format.h>

namespace rex {

namespace {

constexpr size_t kChunkSize = 256 * 1024;

struct StateDeleter {
  void operator()(XXH3_state_t* state) const { XXH3_freeState(state); }
};

std::string HexDigest(XXH128_hash_t digest) {
  return fmt::format("{:016x}{:016x}", digest.high64, digest.low64);
}

}  // namespace

std::string hash_bytes(std::string_view data) {
  return HexDigest(XXH3_128bits(data.data(), data.size()));
}

std::string hash_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};

  std::unique_ptr<XXH3_state_t, StateDeleter> state(XXH3_createState());
  if (!state || XXH3_128bits_reset(state.get()) == XXH_ERROR)
    return {};

  auto chunk = std::make_unique_for_overwrite<char[]>(kChunkSize);
  while (file.read(chunk.get(), kChunkSize) || file.gcount() > 0) {
    const auto got = static_cast<size_t>(file.gcount());
    if (XXH3_128bits_update(state.get(), chunk.get(), got) == XXH_ERROR)
      return {};
  }

  if (file.bad())
    return {};

  return HexDigest(XXH3_128bits_digest(state.get()));
}

}  // namespace rex
