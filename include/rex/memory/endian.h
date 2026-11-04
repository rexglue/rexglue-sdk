/**
 * @file        memory/endian.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/memory.h (Ben Vanik, 2020).
 */

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/types.h>

namespace rex::memory {

/**
 * Copies 16 byte blocks between 16 byte aligned buffers.
 *
 * @param count Number of 16 byte blocks.
 */
REX_API void Copy128Aligned(void* dest, const void* src, size_t count);

/**
 * Copies 16 bit values and swaps the byte order of each. Both buffers are 16 byte aligned.
 *
 * @param count Number of 16 bit values.
 */
REX_API void copy_and_swap_16_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 16 bit values and swaps the byte order of each.
 *
 * @param count Number of 16 bit values.
 */
REX_API void copy_and_swap_16_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the byte order of each. Both buffers are 16 byte aligned.
 *
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_32_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the byte order of each.
 *
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_32_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies 64 bit values and swaps the byte order of each. Both buffers are 16 byte aligned.
 *
 * @param count Number of 64 bit values.
 */
REX_API void copy_and_swap_64_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 64 bit values and swaps the byte order of each.
 *
 * @param count Number of 64 bit values.
 */
REX_API void copy_and_swap_64_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the two 16 bit halves of each. Both buffers are 16 byte aligned.
 *
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_16_in_32_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the two 16 bit halves of each.
 *
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_16_in_32_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies values and swaps the byte order of each.
 *
 * @tparam T Element type, 1, 2, 4 or 8 bytes.
 */
template <typename T>
void copy_and_swap(T* dest, const T* src, size_t count) {
  bool is_aligned =
      reinterpret_cast<uintptr_t>(dest) % 32 == 0 && reinterpret_cast<uintptr_t>(src) % 32 == 0;
  if (sizeof(T) == 1) {
    std::memcpy(dest, src, count);
  } else if (sizeof(T) == 2) {
    auto ps = reinterpret_cast<const u16*>(src);
    auto pd = reinterpret_cast<u16*>(dest);
    if (is_aligned) {
      copy_and_swap_16_aligned(pd, ps, count);
    } else {
      copy_and_swap_16_unaligned(pd, ps, count);
    }
  } else if (sizeof(T) == 4) {
    auto ps = reinterpret_cast<const u32*>(src);
    auto pd = reinterpret_cast<u32*>(dest);
    if (is_aligned) {
      copy_and_swap_32_aligned(pd, ps, count);
    } else {
      copy_and_swap_32_unaligned(pd, ps, count);
    }
  } else if (sizeof(T) == 8) {
    auto ps = reinterpret_cast<const u64*>(src);
    auto pd = reinterpret_cast<u64*>(dest);
    if (is_aligned) {
      copy_and_swap_64_aligned(pd, ps, count);
    } else {
      copy_and_swap_64_unaligned(pd, ps, count);
    }
  } else {
    REX_ASSERT_ALWAYS("Invalid memory::copy_and_swap size");
  }
}

/** Loads a value from memory of any alignment. */
template <typename T>
inline T load(const void* mem) {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  T result;
  std::memcpy(&result, mem, sizeof(T));
  return result;
}

/**
 * Loads a value from memory and swaps its byte order.
 *
 * @tparam T Value type, 1, 2, 4 or 8 bytes.
 * @return The byte swapped value at mem.
 */
template <typename T>
inline T load_and_swap(const void* mem) {
  return byte_swap(load<T>(mem));
}

/**
 * Loads a NUL terminated 8 bit string.
 *
 * @return The characters before the terminator.
 */
template <>
inline std::string load_and_swap<std::string>(const void* mem) {
  std::string value;
  for (i32 i = 0;; ++i) {
    auto c = load_and_swap<u8>(reinterpret_cast<const u8*>(mem) + i);
    if (!c) {
      break;
    }
    value.push_back(static_cast<char>(c));
  }
  return value;
}

/**
 * Loads a NUL terminated big endian UTF-16 string.
 *
 * @return The code units before the terminator in host byte order.
 */
template <>
inline std::u16string load_and_swap<std::u16string>(const void* mem) {
  std::u16string value;
  for (i32 i = 0;; ++i) {
    auto c = load_and_swap<u16>(reinterpret_cast<const u16*>(mem) + i);
    if (!c) {
      break;
    }
    value.push_back(static_cast<wchar_t>(c));
  }
  return value;
}

/** Stores a value to memory of any alignment. */
template <typename T>
inline void store(void* mem, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  std::memcpy(mem, &value, sizeof(T));
}

/**
 * Swaps the byte order of a value and stores it to memory.
 *
 * @tparam T Value type, 1, 2, 4 or 8 bytes.
 */
template <typename T>
inline void store_and_swap(void* mem, const T& value) {
  store(mem, byte_swap(value));
}

/** Stores the characters of an 8 bit string without a terminator. */
template <>
inline void store_and_swap<std::string_view>(void* mem, const std::string_view& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    store_and_swap<u8>(reinterpret_cast<u8*>(mem) + i, value[i]);
  }
}

/** Stores the characters of an 8 bit string without a terminator. */
template <>
inline void store_and_swap<std::string>(void* mem, const std::string& value) {
  return store_and_swap<std::string_view>(mem, value);
}

/** Stores a UTF-16 string as big endian code units without a terminator. */
template <>
inline void store_and_swap<std::u16string_view>(void* mem, const std::u16string_view& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    store_and_swap<u16>(reinterpret_cast<u16*>(mem) + i, value[i]);
  }
}

/** Stores a UTF-16 string as big endian code units without a terminator. */
template <>
inline void store_and_swap<std::u16string>(void* mem, const std::u16string& value) {
  return store_and_swap<std::u16string_view>(mem, value);
}

/**
 * Reads a 24 bit big endian value.
 *
 * @param p The 3 source bytes.
 */
constexpr u32 LoadUint24BE(const u8* p) {
  return (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | static_cast<u32>(p[2]);
}

/**
 * Reads a 24 bit little endian value.
 *
 * @param p The 3 source bytes.
 */
constexpr u32 LoadUint24LE(const u8* p) {
  return (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[1]) << 8) | static_cast<u32>(p[0]);
}

/**
 * Writes a 24 bit little endian value.
 *
 * @param p The 3 destination bytes.
 * @param value Value whose top 8 bits are discarded.
 */
constexpr void StoreUint24LE(u8* p, u32 value) {
  p[2] = static_cast<u8>((value >> 16) & 0xFF);
  p[1] = static_cast<u8>((value >> 8) & 0xFF);
  p[0] = static_cast<u8>(value & 0xFF);
}

/** 4 character code packed into 32 bits. */
using FourCCT = u32;

/**
 * Packs 4 characters into a FourCC in host byte order.
 *
 * @param a First character, the most significant byte.
 * @param d Fourth character, the least significant byte.
 * @return The packed code. make_fourcc('a', 'b', 'c', 'd') is 0x61626364.
 */
constexpr FourCCT make_fourcc(char a, char b, char c, char d) {
  return ((static_cast<FourCCT>(a) << 24) | (static_cast<FourCCT>(b) << 16) |
          (static_cast<FourCCT>(c) << 8) | static_cast<FourCCT>(d));
}

/**
 * Packs a 4 character string into a FourCC in host byte order.
 *
 * @param fourcc String of exactly 4 characters. Any other length throws std::runtime_error.
 * @return The packed code. make_fourcc("abcd") is 0x61626364.
 */
constexpr FourCCT make_fourcc(const std::string_view fourcc) {
  if (fourcc.length() != 4) {
    throw std::runtime_error("Invalid fourcc length");
  }
  return make_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

}  // namespace rex::memory
