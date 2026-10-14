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
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <rex/macros.h>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif

#include <xxhash.h>

namespace rex {

/**
 * Hasher that returns a key already hashed elsewhere, for containers with external hashing.
 *
 * @tparam Key Key type convertible to size_t.
 */
template <typename Key>
struct IdentityHasher {
  /**
   * @param key The precomputed hash.
   * @return key cast to size_t.
   */
  size_t operator()(const Key& key) const { return static_cast<size_t>(key); }
};

/**
 * Hasher that runs XXH3 over the object representation of the key.
 *
 * @tparam Key Trivially copyable key type.
 */
template <typename Key>
struct XXHasher {
  /**
   * @param key The key to hash.
   * @return The 64 bit XXH3 hash of the bytes of key.
   */
  size_t operator()(const Key& key) const {
    return static_cast<size_t>(XXH3_64bits(&key, sizeof(key)));
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
 * 128 bit XXH3 digest over the contents of a file, read in chunks.
 *
 * @param path The file to hash.
 * @return The digest as a 32 character lowercase hex string, or an empty string when the path is
 *         not a regular file or cannot be opened or read.
 */
REX_API std::string HashFile(const std::filesystem::path& path);

/**
 * Ends the hash_combine recursion.
 *
 * @param hash Hash value so far.
 * @return The hash value unchanged.
 */
inline size_t hash_combine(size_t hash) {
  return hash;
}

/**
 * Mixes the std::hash of each value into a hash value.
 *
 * @tparam T Type of the first value.
 * @tparam Ts Types of the remaining values.
 * @param hash Hash value to mix into.
 * @param v First value.
 * @param vs Remaining values.
 * @return The combined hash value.
 */
template <typename T, typename... Ts>
size_t hash_combine(size_t hash, const T& v, const Ts&... vs) {
  std::hash<T> hasher;
  hash ^= hasher(v) + 0x9E3779B9 + (hash << 6) + (hash >> 2);
  return hash_combine(hash, vs...);
}

}  // namespace rex
