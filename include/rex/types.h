/**
 * @file        types.h
 * @brief       Core type definitions and aliases
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <rex/assert.h>
#include <rex/platform.h>

// Check for mixed endian
static_assert((std::endian::native == std::endian::big) ||
              (std::endian::native == std::endian::little));

namespace rex {

/// Byte-swap a value of any trivially copyable type (1, 2, 4, or 8 bytes).
/// Uses std::bit_cast for safe type punning and std::byteswap for the swap.
template <class T>
constexpr T byte_swap(T value) noexcept {
  static_assert(sizeof(T) == 8 || sizeof(T) == 4 || sizeof(T) == 2 || sizeof(T) == 1,
                "byte_swap(T value): Type T has illegal size");
  if constexpr (sizeof(T) == 1) {
    return value;
  } else {
    // Convert to unsigned integer of same size, byteswap, convert back
    using uint_t = std::conditional_t<sizeof(T) == 8, uint64_t,
                                      std::conditional_t<sizeof(T) == 4, uint32_t, uint16_t>>;
    return std::bit_cast<T>(std::byteswap(std::bit_cast<uint_t>(value)));
  }
}

template <typename T, std::endian E>
struct endian_store {
  using value_type = T;  // Type alias for value() in PPCPointer

  endian_store() = default;
  endian_store(const T& src) { set(src); }
  endian_store(const endian_store&) = default;
  endian_store& operator=(const endian_store&) = default;
  operator T() const { return get(); }

  void set(const T& src) {
    if constexpr (std::endian::native == E) {
      value = src;
    } else {
      value = rex::byte_swap(src);
    }
  }
  T get() const {
    if constexpr (std::endian::native == E) {
      return value;
    }
    return rex::byte_swap(value);
  }

  endian_store<T, E>& operator+=(int a) {
    *this = *this + a;
    return *this;
  }
  endian_store<T, E>& operator-=(int a) {
    *this = *this - a;
    return *this;
  }
  endian_store<T, E>& operator++() {
    *this += 1;
    return *this;
  }  // ++a
  endian_store<T, E> operator++(int) {
    *this += 1;
    return (*this - 1);
  }  // a++
  endian_store<T, E>& operator--() {
    *this -= 1;
    return *this;
  }  // --a
  endian_store<T, E> operator--(int) {
    *this -= 1;
    return (*this + 1);
  }  // a--

  T value;
};

template <typename T>
using be = endian_store<T, std::endian::big>;
template <typename T>
using le = endian_store<T, std::endian::little>;

//=============================================================================
// Basic Integer Types
//=============================================================================

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

static_assert(sizeof(f32) == 4, "float must be 4 bytes");
static_assert(sizeof(f64) == 8, "double must be 8 bytes");

//=============================================================================
// Memory Address Types
//=============================================================================

using guest_addr_t = u32;       // Xbox 360 guest address (32-bit)
using host_addr_t = uintptr_t;  // Host native address (64-bit on x64)

//=============================================================================
// Big-Endian Type Aliases (using rex::be<T>)
//=============================================================================

using be_u8 = u8;  // No byte-swapping needed for single bytes
using be_u16 = be<u16>;
using be_u32 = be<u32>;
using be_u64 = be<u64>;

using be_i8 = i8;
using be_i16 = be<i16>;
using be_i32 = be<i32>;
using be_i64 = be<i64>;

using be_f32 = be<f32>;
using be_f64 = be<f64>;

}  // namespace rex
