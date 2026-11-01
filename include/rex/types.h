/**
 * @file        types.h
 * @brief       Integer types, byte order wrappers and size literals
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/literals.h (Ben Vanik, 2021).
 */

#pragma once

#include <atomic>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <rex/assert.h>
#include <rex/platform.h>

static_assert((std::endian::native == std::endian::big) ||
              (std::endian::native == std::endian::little));

namespace rex {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using f32 = float;
using f64 = double;

static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");

/** Reverses the byte order of a trivially copyable value of 1, 2, 4 or 8 bytes. */
template <class T>
constexpr T byte_swap(T value) noexcept {
  static_assert(sizeof(T) == 8 || sizeof(T) == 4 || sizeof(T) == 2 || sizeof(T) == 1,
                "byte_swap(T value): Type T has illegal size");
  if constexpr (sizeof(T) == 1) {
    return value;
  } else {
    using UintT =
        std::conditional_t<sizeof(T) == 8, u64, std::conditional_t<sizeof(T) == 4, u32, u16>>;
    return std::bit_cast<T>(std::byteswap(std::bit_cast<UintT>(value)));
  }
}

/** Value held in memory in a fixed byte order and converted to native order on access. */
template <typename T, std::endian E>
struct EndianValue {
  using value_type = T;

  EndianValue() = default;

  EndianValue(const T& src) { Set(src); }

  EndianValue(const EndianValue&) = default;
  EndianValue& operator=(const EndianValue&) = default;

  /**
   * @return The value in native byte order.
   */
  operator T() const { return Get(); }

  /** Stores a native value in byte order E. */
  void Set(const T& src) {
    if constexpr (std::endian::native == E) {
      value = src;
    } else {
      value = rex::byte_swap(src);
    }
  }

  /**
   * @return The value in native byte order.
   */
  T Get() const {
    if constexpr (std::endian::native == E) {
      return value;
    }
    return rex::byte_swap(value);
  }

  /**
   * Loads the native value atomically.
   *
   * @return The value in native byte order.
   */
  T Load(std::memory_order order) const {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    REX_ASSERT(std::bit_cast<uintptr_t>(&value) % std::atomic_ref<T>::required_alignment == 0);
    const T raw = std::atomic_ref<T>(const_cast<T&>(value)).load(order);
    if constexpr (std::endian::native == E) {
      return raw;
    }
    return rex::byte_swap(raw);
  }

  /** Stores a native value in byte order E atomically. */
  void Store(T src, std::memory_order order) {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    REX_ASSERT(std::bit_cast<uintptr_t>(&value) % std::atomic_ref<T>::required_alignment == 0);
    if constexpr (std::endian::native != E) {
      src = rex::byte_swap(src);
    }
    std::atomic_ref<T>(value).store(src, order);
  }

  /**
   * Replaces the value atomically when it equals an expected native value.
   *
   * @param expected The native value to compare. Receives the current native value on failure.
   * @param desired The native value to store on success.
   * @return True when the value matched and desired was stored.
   */
  bool CompareExchange(T& expected, T desired, std::memory_order order) {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    REX_ASSERT(std::bit_cast<uintptr_t>(&value) % std::atomic_ref<T>::required_alignment == 0);
    T raw_expected = expected;
    if constexpr (std::endian::native != E) {
      raw_expected = rex::byte_swap(expected);
      desired = rex::byte_swap(desired);
    }
    if (std::atomic_ref<T>(value).compare_exchange_strong(raw_expected, desired, order)) {
      return true;
    }
    if constexpr (std::endian::native != E) {
      expected = rex::byte_swap(raw_expected);
    } else {
      expected = raw_expected;
    }
    return false;
  }

  EndianValue<T, E>& operator+=(int a) {
    *this = *this + a;
    return *this;
  }

  EndianValue<T, E>& operator-=(int a) {
    *this = *this - a;
    return *this;
  }

  EndianValue<T, E>& operator++() {
    *this += 1;
    return *this;
  }

  EndianValue<T, E> operator++(int) {
    *this += 1;
    return (*this - 1);
  }

  EndianValue<T, E>& operator--() {
    *this -= 1;
    return *this;
  }

  EndianValue<T, E> operator--(int) {
    *this -= 1;
    return (*this + 1);
  }

  T value; /**< The raw value in byte order E. */
};

template <typename T>
using BE = EndianValue<T, std::endian::big>;

template <typename T>
using LE = EndianValue<T, std::endian::little>;

template <typename T>
struct IsBEType : std::false_type {};

template <typename T>
struct IsBEType<rex::BE<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_be_type_v = IsBEType<T>::value;

using GuestAddrT = u32;
using host_addr_t = uintptr_t;

/** Single byte, which needs no byte swap. */
using be_u8 = u8;
using be_u16 = BE<u16>;
using be_u32 = BE<u32>;
using be_u64 = BE<u64>;

/** Single byte, which needs no byte swap. */
using be_i8 = i8;
using be_i16 = BE<i16>;
using be_i32 = BE<i32>;
using be_i64 = BE<i64>;

using be_f32 = BE<f32>;
using be_f64 = BE<f64>;

}  // namespace rex

using u8 = rex::u8;
using i8 = rex::i8;
using u16 = rex::u16;
using i16 = rex::i16;
using u32 = rex::u32;
using i32 = rex::i32;
using u64 = rex::u64;
using i64 = rex::i64;
using f32 = rex::f32;
using f64 = rex::f64;

using be_u16 = rex::be_u16;
using be_u32 = rex::be_u32;
using be_u64 = rex::be_u64;
using be_i16 = rex::be_i16;
using be_i32 = rex::be_i32;
using be_i64 = rex::be_i64;
using be_f32 = rex::be_f32;
using be_f64 = rex::be_f64;

namespace rex {

/** Returns a size in bytes from a count of kibibytes. */
constexpr size_t operator""_KiB(unsigned long long int x) {
  return 1024ULL * x;
}

/** Returns a size in bytes from a count of mebibytes. */
constexpr size_t operator""_MiB(unsigned long long int x) {
  return 1024_KiB * x;
}

/** Returns a size in bytes from a count of gibibytes. */
constexpr size_t operator""_GiB(unsigned long long int x) {
  return 1024_MiB * x;
}

/** Returns a size in bytes from a count of tebibytes. */
constexpr size_t operator""_TiB(unsigned long long int x) {
  return 1024_GiB * x;
}

/** Returns a size in bytes from a count of pebibytes. */
constexpr size_t operator""_PiB(unsigned long long int x) {
  return 1024_TiB * x;
}

}  // namespace rex
