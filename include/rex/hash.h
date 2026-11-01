/**
 * @file        hash.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/hash.h (Ben Vanik, 2020).
 */

#pragma once

#include <functional>
#include <span>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>

#include <rex/types.h>
#include <rex/platform.h>

namespace rex {

/**
 * Hasher that returns a key hashed elsewhere, for containers with external hashing.
 *
 * @tparam Key Key type convertible to size_t.
 */
template <typename Key>
struct IdentityHasher {
  /**
   * @return key cast to size_t.
   */
  size_t operator()(const Key& key) const { return static_cast<size_t>(key); }
};

/**
 * Hasher for a key type that holds its identity in a member named key, hashed with std::hash.
 *
 * @tparam Key Key type with a hashable member key.
 */
template <typename Key>
struct KeyHasher {
  /**
   * @return std::hash of the key member.
   */
  size_t operator()(const Key& hashed_key) const {
    return std::hash<std::remove_cvref_t<decltype(hashed_key.key)>>{}(hashed_key.key);
  }
};

/**
 * 64 bit XXH3 hash for hash tables and keys, not for security use.
 *
 * @param data The bytes to hash.
 */
REX_API u64 HashXXH3(std::span<const u8> data);

/** Hasher that runs XXH3 over the object representation of the key. */
template <typename Key>
struct XXHasher {
  /**
   * @return The 64 bit XXH3 hash of the bytes of key.
   */
  size_t operator()(const Key& key) const {
    return static_cast<size_t>(
        HashXXH3(std::span<const u8>(reinterpret_cast<const u8*>(&key), sizeof(key))));
  }
};

/**
 * 128 bit XXH3 digest for fingerprints and cache keys, not for security use.
 *
 * @param data The bytes to hash.
 * @return The digest as a 32 character lowercase hex string.
 */
REX_API std::string HashBytes(std::string_view data);

/**
 * 128 bit XXH3 digest over the contents of a file.
 *
 * @return The digest as a 32 character lowercase hex string, or an empty string when the path is
 *         not a regular file or cannot be opened or read.
 */
REX_API std::string HashFile(const std::filesystem::path& path);

/**
 * Ends the hash_combine recursion.
 *
 * @return The hash value unchanged.
 */
inline size_t hash_combine(size_t hash) {
  return hash;
}

/** Mixes the std::hash of each value into a hash value. */
template <typename T, typename... Ts>
size_t hash_combine(size_t hash, const T& v, const Ts&... vs) {
  std::hash<T> hasher;
  hash ^= hasher(v) + 0x9E3779B9 + (hash << 6) + (hash >> 2);
  return hash_combine(hash, vs...);
}

}  // namespace rex
