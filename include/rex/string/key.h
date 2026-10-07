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
#include <variant>

#include <rex/string/utf8.h>

namespace rex {

namespace detail {

struct string_key_base {
 private:
  std::variant<std::string, std::string_view> value_;

 public:
  explicit string_key_base(const std::string_view value) : value_(value) {}
  explicit string_key_base(std::string value) : value_(std::move(value)) {}

  std::string_view view() const {
    return std::holds_alternative<std::string>(value_) ? std::get<std::string>(value_)
                                                       : std::get<std::string_view>(value_);
  }
};

}  // namespace detail

/**
 * Case sensitive string key. It either borrows a view or owns a copy. Borrow for lookups and own
 * for stored keys.
 */
struct string_key : detail::string_key_base {
 public:
  /**
   * Borrows a string. The string outlives the key.
   *
   * @param value String to borrow.
   */
  explicit string_key(const std::string_view value) : string_key_base(value) {}

  /**
   * Owns a string.
   *
   * @param value String to own.
   */
  explicit string_key(std::string value) : string_key_base(value) {}

  /**
   * Creates a key that owns a copy of a string.
   *
   * @param value String to copy.
   * @return The owning key.
   */
  static string_key create(const std::string_view value) { return string_key(std::string(value)); }

  /**
   * Creates a key that owns a string.
   *
   * @param value String to own.
   * @return The owning key.
   */
  static string_key create(std::string value) { return string_key(value); }

  /**
   * Compares 2 keys byte for byte.
   *
   * @param other Key to compare with.
   * @return True when the strings are equal.
   */
  bool operator==(const string_key& other) const { return other.view() == view(); }

  /**
   * Hashes the key.
   *
   * @return FNV-1a hash of the code points.
   */
  size_t hash() const { return rex::utf8_hash_fnv1a(view()); }

  /** Hash functor for unordered containers. */
  struct Hash {
    /**
     * Hashes a key.
     *
     * @param t Key to hash.
     * @return t.hash().
     */
    size_t operator()(const string_key& t) const { return t.hash(); }
  };
};

/**
 * String key that compares and hashes ignoring ASCII case. It either borrows a view or owns a
 * copy.
 */
struct string_key_case : detail::string_key_base {
 public:
  /**
   * Borrows a string. The string outlives the key.
   *
   * @param value String to borrow.
   */
  explicit string_key_case(const std::string_view value) : string_key_base(value) {}

  /**
   * Owns a string.
   *
   * @param value String to own.
   */
  explicit string_key_case(std::string value) : string_key_base(value) {}

  /**
   * Creates a key that owns a copy of a string.
   *
   * @param value String to copy.
   * @return The owning key.
   */
  static string_key_case create(const std::string_view value) {
    return string_key_case(std::string(value));
  }

  /**
   * Creates a key that owns a string.
   *
   * @param value String to own.
   * @return The owning key.
   */
  static string_key_case create(std::string value) { return string_key_case(value); }

  /**
   * Compares 2 keys, ignoring ASCII case.
   *
   * @param other Key to compare with.
   * @return True when the strings match.
   */
  bool operator==(const string_key_case& other) const {
    return rex::utf8_equal_case(other.view(), view());
  }

  /**
   * Hashes the key, ignoring ASCII case.
   *
   * @return FNV-1a hash of the code points with ASCII letters lowercased.
   */
  size_t hash() const { return rex::utf8_hash_fnv1a_case(view()); }

  /** Hash functor for unordered containers. */
  struct Hash {
    /**
     * Hashes a key.
     *
     * @param t Key to hash.
     * @return t.hash().
     */
    size_t operator()(const string_key_case& t) const { return t.hash(); }
  };
};

}  // namespace rex

namespace std {
/** std::hash for rex::string_key. */
template <>
struct hash<rex::string_key> {
  /**
   * Hashes a key.
   *
   * @param t Key to hash.
   * @return t.hash().
   */
  std::size_t operator()(const rex::string_key& t) const { return t.hash(); }
};

/** std::hash for rex::string_key_case. */
template <>
struct hash<rex::string_key_case> {
  /**
   * Hashes a key.
   *
   * @param t Key to hash.
   * @return t.hash().
   */
  std::size_t operator()(const rex::string_key_case& t) const { return t.hash(); }
};
}  // namespace std
