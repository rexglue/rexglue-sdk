/**
 * @file        string/key.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/string_key.h (Ben Vanik, 2020).
 */

#pragma once

#include <string>
#include <utility>
#include <variant>

#include <rex/string/utf8.h>

namespace rex {

namespace detail {

struct StringKeyBase {
 private:
  std::variant<std::string, std::string_view> value_;

 public:
  explicit StringKeyBase(const std::string_view value) : value_(value) {}
  explicit StringKeyBase(std::string value) : value_(std::move(value)) {}

  std::string_view GetView() const {
    if (const auto* owned = std::get_if<std::string>(&value_)) {
      return *owned;
    }
    const auto* borrowed = std::get_if<std::string_view>(&value_);
    return borrowed ? *borrowed : std::string_view();
  }
};

}  // namespace detail

/**
 * Case sensitive string key. It either borrows a view or owns a copy.
 */
struct StringKey : detail::StringKeyBase {
 public:
  /**
   * Borrows a string. The string outlives the key.
   *
   * @param value String to borrow.
   */
  explicit StringKey(const std::string_view value) : StringKeyBase(value) {}

  /**
   * Owns a string.
   *
   * @param value String to own.
   */
  explicit StringKey(std::string value) : StringKeyBase(std::move(value)) {}

  /**
   * Creates a key that owns a copy of a string.
   *
   * @param value String to copy.
   * @return The owning key.
   */
  static StringKey Create(const std::string_view value) { return StringKey(std::string(value)); }

  /**
   * Creates a key that owns a string.
   *
   * @param value String to own.
   * @return The owning key.
   */
  static StringKey Create(std::string value) { return StringKey(std::move(value)); }

  /**
   * Compares 2 keys byte for byte.
   *
   * @param other Key to compare with.
   * @return True when the strings are equal.
   */
  bool operator==(const StringKey& other) const { return other.GetView() == GetView(); }

  /**
   * Hashes the key.
   *
   * @return FNV-1a hash of the code points.
   */
  size_t hash() const { return rex::utf8_hash_fnv1a(GetView()); }

  /** Hash functor for unordered containers. */
  struct Hash {
    /**
     * Hashes a key.
     *
     * @param t Key to hash.
     * @return t.hash().
     */
    size_t operator()(const StringKey& t) const { return t.hash(); }
  };
};

/**
 * String key that compares and hashes ignoring ASCII case. It either borrows a view or owns a
 * copy.
 */
struct StringKeyIgnoreCase : detail::StringKeyBase {
 public:
  /**
   * Borrows a string. The string outlives the key.
   *
   * @param value String to borrow.
   */
  explicit StringKeyIgnoreCase(const std::string_view value) : StringKeyBase(value) {}

  /**
   * Owns a string.
   *
   * @param value String to own.
   */
  explicit StringKeyIgnoreCase(std::string value) : StringKeyBase(std::move(value)) {}

  /**
   * Creates a key that owns a copy of a string.
   *
   * @param value String to copy.
   * @return The owning key.
   */
  static StringKeyIgnoreCase Create(const std::string_view value) {
    return StringKeyIgnoreCase(std::string(value));
  }

  /**
   * Creates a key that owns a string.
   *
   * @param value String to own.
   * @return The owning key.
   */
  static StringKeyIgnoreCase Create(std::string value) {
    return StringKeyIgnoreCase(std::move(value));
  }

  /**
   * Compares 2 keys, ignoring ASCII case.
   *
   * @param other Key to compare with.
   * @return True when the strings match.
   */
  bool operator==(const StringKeyIgnoreCase& other) const {
    return rex::utf8_equal_case(other.GetView(), GetView());
  }

  /**
   * Hashes the key, ignoring ASCII case.
   *
   * @return FNV-1a hash of the code points with ASCII letters lowercased.
   */
  size_t hash() const { return rex::utf8_hash_fnv1a_case(GetView()); }

  /** Hash functor for unordered containers. */
  struct Hash {
    /**
     * Hashes a key.
     *
     * @param t Key to hash.
     * @return t.hash().
     */
    size_t operator()(const StringKeyIgnoreCase& t) const { return t.hash(); }
  };
};

}  // namespace rex

namespace std {
/** std::hash for rex::StringKey. */
template <>
struct hash<rex::StringKey> {
  /**
   * Hashes a key.
   *
   * @param t Key to hash.
   * @return t.hash().
   */
  std::size_t operator()(const rex::StringKey& t) const { return t.hash(); }
};

/** std::hash for rex::StringKeyIgnoreCase. */
template <>
struct hash<rex::StringKeyIgnoreCase> {
  /**
   * Hashes a key.
   *
   * @param t Key to hash.
   * @return t.hash().
   */
  std::size_t operator()(const rex::StringKeyIgnoreCase& t) const { return t.hash(); }
};
}  // namespace std
