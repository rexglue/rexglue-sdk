/**
 * @file        bit.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/bit_map.h and xenia/base/bit_range.h
 *              (Ben Vanik, 2015-2019).
 */

#pragma once

#include <algorithm>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

/** Reports whether a value is a power of 2 or 0. */
template <typename T>
constexpr bool is_pow2(T value) {
  return (value & (value - 1)) == 0;
}

/**
 * Returns the smallest power of 2 greater than or equal to a value.
 *
 * @tparam T Unsigned integer type up to 32 bits.
 */
template <typename T>
T next_pow2(T value) {
  value--;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value++;
  return value;
}

/**
 * Returns a mask with a contiguous range of bits set.
 *
 * @param a Index of the lowest set bit.
 * @param b Index of the highest set bit.
 */
constexpr u32 MakeBitmask(u32 a, u32 b) {
  return (static_cast<u32>(-1) >> (31 - b)) & ~((1u << a) - 1);
}

/**
 * Returns a range of bits of a value, shifted down to bit 0.
 *
 * @param a Index of the lowest bit to extract.
 * @param b Index of the highest bit to extract.
 */
constexpr u32 select_bits(u32 value, u32 a, u32 b) {
  return (value & MakeBitmask(a, b)) >> a;
}

/**
 * Returns the number of set bits in a value.
 *
 * @tparam T Unsigned integer type.
 */
template <class T>
constexpr u32 BitCount(T v) {
  return static_cast<u32>(std::popcount(v));
}

/** Returns the number of leading zero bits in an 8 bit value, or 8 when v is 0. */
inline u8 lzcnt(u8 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/** Returns the number of leading zero bits in a 16 bit value, or 16 when v is 0. */
inline u8 lzcnt(u16 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/** Returns the number of leading zero bits in a 32 bit value, or 32 when v is 0. */
inline u8 lzcnt(u32 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/** Returns the number of leading zero bits in a 64 bit value, or 64 when v is 0. */
inline u8 lzcnt(u64 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/** Returns the number of trailing zero bits in an 8 bit value, or 8 when v is 0. */
inline u8 tzcnt(u8 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/** Returns the number of trailing zero bits in a 16 bit value, or 16 when v is 0. */
inline u8 tzcnt(u16 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/** Returns the number of trailing zero bits in a 32 bit value, or 32 when v is 0. */
inline u8 tzcnt(u32 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/** Returns the number of trailing zero bits in a 64 bit value, or 64 when v is 0. */
inline u8 tzcnt(u64 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/**
 * Returns the number of leading zero bits in a signed 8 bit value, or 8 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 lzcnt(i8 v) {
  return lzcnt(static_cast<u8>(v));
}

/**
 * Returns the number of leading zero bits in a signed 16 bit value, or 16 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 lzcnt(i16 v) {
  return lzcnt(static_cast<u16>(v));
}

/**
 * Returns the number of leading zero bits in a signed 32 bit value, or 32 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 lzcnt(i32 v) {
  return lzcnt(static_cast<u32>(v));
}

/**
 * Returns the number of leading zero bits in a signed 64 bit value, or 64 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 lzcnt(i64 v) {
  return lzcnt(static_cast<u64>(v));
}

/**
 * Returns the number of trailing zero bits in a signed 8 bit value, or 8 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 tzcnt(i8 v) {
  return tzcnt(static_cast<u8>(v));
}

/**
 * Returns the number of trailing zero bits in a signed 16 bit value, or 16 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 tzcnt(i16 v) {
  return tzcnt(static_cast<u16>(v));
}

/**
 * Returns the number of trailing zero bits in a signed 32 bit value, or 32 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 tzcnt(i32 v) {
  return tzcnt(static_cast<u32>(v));
}

/**
 * Returns the number of trailing zero bits in a signed 64 bit value, or 64 when v is 0.
 *
 * @param v The value to scan, read as unsigned.
 */
inline u8 tzcnt(i64 v) {
  return tzcnt(static_cast<u64>(v));
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(u32 v, u32* out_first_set_index) {
  if (v == 0) {
    return false;
  }
  *out_first_set_index = static_cast<u32>(std::countr_zero(v));
  return true;
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(u64 v, u32* out_first_set_index) {
  if (v == 0) {
    return false;
  }
  *out_first_set_index = static_cast<u32>(std::countr_zero(v));
  return true;
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param v The value to scan, read as unsigned.
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(i32 v, u32* out_first_set_index) {
  return bit_scan_forward(static_cast<u32>(v), out_first_set_index);
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param v The value to scan, read as unsigned.
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(i64 v, u32* out_first_set_index) {
  return bit_scan_forward(static_cast<u64>(v), out_first_set_index);
}

/**
 * Returns the base 2 logarithm of a value rounded down.
 *
 * @param v A value greater than 0.
 */
template <typename T>
inline T log2_floor(T v) {
  return sizeof(T) * 8 - 1 - lzcnt(v);
}

/**
 * Returns the base 2 logarithm of a value rounded up.
 *
 * @param v A value greater than 0.
 */
template <typename T>
inline T log2_ceil(T v) {
  return sizeof(T) * 8 - lzcnt(v - 1);
}

/**
 * Rotates a value left.
 *
 * @tparam T Unsigned integer type.
 * @param sh Rotation amount in bits, between 1 and the bit width of T minus 1.
 */
template <typename T>
inline T RotateLeft(T v, u8 sh) {
  return (v << sh) | (v >> ((sizeof(T) * 8) - sh));
}

/** Bitmap of free and used entries where a set bit marks a free entry. */
class REX_API BitMap {
 public:
  BitMap();

  /**
   * Creates a bitmap with every entry free.
   *
   * @param size_bits Number of entries, a multiple of 64.
   */
  BitMap(size_t size_bits);

  /**
   * Creates a bitmap initialized from existing words.
   *
   * @param data Source words, 8 byte alignment preferred but not required.
   * @param size_bits Number of entries, a multiple of 64.
   */
  BitMap(u64* data, size_t size_bits);

  /**
   * Atomically marks the first free entry as used. Thread safe.
   *
   * @return The index of the acquired entry, or size_t(-1) when no entry is free.
   */
  size_t AcquireEntry();

  /** Atomically marks an entry as free. Thread safe. */
  void ReleaseEntry(size_t index);

  /**
   * Resizes the bitmap. New entries start free.
   *
   * @param new_size_bits Number of entries, a multiple of 64.
   */
  void Resize(size_t new_size_bits);

  void Reset();

  /** Returns a copy of the backing words. */
  const std::vector<u64> GetWords() const { return data_; }

  std::vector<u64>& GetWords() { return data_; }

 private:
  const static size_t kDataSize = 8;
  const static size_t kDataSizeBits = kDataSize * 8;
  std::vector<u64> data_;
};

/**
 * Finds the first run of clear bits in a range.
 *
 * @tparam Block Unsigned word type of the bit array.
 * @param bits The bit array.
 * @param first Index of the first bit to search.
 * @param length Number of bits to search starting at first.
 * @return The start and length in bits of the first clear run, or a length of 0 when none exists.
 */
template <typename Block>
std::pair<size_t, size_t> GetNextRangeUnset(const Block* bits, size_t first, size_t length) {
  if (!length) {
    return std::make_pair(first, static_cast<size_t>(0));
  }
  size_t last = first + length - 1;
  const size_t block_bits = sizeof(Block) * CHAR_BIT;
  size_t block_first = first / block_bits;
  size_t block_last = last / block_bits;
  size_t range_start = SIZE_MAX;
  for (size_t i = block_first; i <= block_last; ++i) {
    Block block = bits[i];
    if (i == block_first) {
      block |= (Block(1) << (first & (block_bits - 1))) - 1;
    }
    if (i == block_last && (last & (block_bits - 1)) != block_bits - 1) {
      block |= ~((Block(1) << ((last & (block_bits - 1)) + 1)) - 1);
    }
    while (true) {
      u32 block_bit;
      if (range_start == SIZE_MAX) {
        if (!rex::bit_scan_forward(~block, &block_bit)) {
          break;
        }
        range_start = i * block_bits + block_bit;
      } else {
        Block block_bits_set_from_start = block;
        if (i == range_start / block_bits) {
          block_bits_set_from_start &= ~((Block(1) << (range_start & (block_bits - 1))) - 1);
        }
        if (!rex::bit_scan_forward(block_bits_set_from_start, &block_bit)) {
          break;
        }
        return std::make_pair(range_start, (i * block_bits) + block_bit - range_start);
      }
    }
  }
  if (range_start != SIZE_MAX) {
    return std::make_pair(range_start, last + static_cast<size_t>(1) - range_start);
  }
  return std::make_pair(first + length, static_cast<size_t>(0));
}

/**
 * Sets every bit in a range.
 *
 * @tparam Block Unsigned word type of the bit array.
 * @param bits The bit array.
 * @param first Index of the first bit to set.
 * @param length Number of bits to set starting at first.
 */
template <typename Block>
void SetRange(Block* bits, size_t first, size_t length) {
  if (!length) {
    return;
  }
  size_t last = first + length - 1;
  const size_t block_bits = sizeof(Block) * CHAR_BIT;
  size_t block_first = first / block_bits;
  size_t block_last = last / block_bits;
  Block set_first = ~((Block(1) << (first & (block_bits - 1))) - 1);
  Block set_last = ~Block(0);
  if ((last & (block_bits - 1)) != (block_bits - 1)) {
    set_last &= (Block(1) << ((last & (block_bits - 1)) + 1)) - 1;
  }
  if (block_first == block_last) {
    bits[block_first] |= set_first & set_last;
    return;
  }
  bits[block_first] |= set_first;
  if (block_first + 1 < block_last) {
    std::fill_n(bits + block_first + 1, block_last - (block_first + 1), ~Block(0));
  }
  bits[block_last] |= set_last;
}

/**
 * Opts an enum into the flag operators.
 *
 * @code
 * template <>
 * inline constexpr bool rex::is_flags_enum<rex::filesystem::MountFlags> = true;
 * @endcode
 */
template <typename E>
inline constexpr bool is_flags_enum = false;

/** An enum that opted into the flag operators through is_flags_enum. */
template <typename E>
concept FlagsEnum = std::is_enum_v<E> && is_flags_enum<E>;

template <FlagsEnum E>
[[nodiscard]] constexpr E operator|(E lhs, E rhs) {
  return static_cast<E>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

template <FlagsEnum E>
[[nodiscard]] constexpr E operator&(E lhs, E rhs) {
  return static_cast<E>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

template <FlagsEnum E>
[[nodiscard]] constexpr E operator^(E lhs, E rhs) {
  return static_cast<E>(std::to_underlying(lhs) ^ std::to_underlying(rhs));
}

template <FlagsEnum E>
[[nodiscard]] constexpr E operator~(E value) {
  return static_cast<E>(~std::to_underlying(value));
}

/**
 * Adds flags to a set in place.
 *
 * @return lhs.
 */
template <FlagsEnum E>
constexpr E& operator|=(E& lhs, E rhs) {
  lhs = lhs | rhs;
  return lhs;
}

/**
 * Keeps only the flags of a set that another set holds, in place.
 *
 * @return lhs.
 */
template <FlagsEnum E>
constexpr E& operator&=(E& lhs, E rhs) {
  lhs = lhs & rhs;
  return lhs;
}

/**
 * Toggles flags of a set in place.
 *
 * @return lhs.
 */
template <FlagsEnum E>
constexpr E& operator^=(E& lhs, E rhs) {
  lhs = lhs ^ rhs;
  return lhs;
}

/** Reports whether a set holds every flag of another set. */
template <FlagsEnum E>
[[nodiscard]] constexpr bool HasFlags(E value, E flags) {
  return (value & flags) == flags;
}

/**
 * Restrict qualifier for variable declarations. Not for return values or the this pointer.
 *
 * @code
 * void Copy(u8* REX_RESTRICT_VAR dst, const u8* REX_RESTRICT_VAR src);
 * @endcode
 */
#define REX_RESTRICT_VAR __restrict

/** Copies the bits of one trivially copyable object into another of the same size. */
template <typename Dst, typename Src>
void Reinterpret(Dst& REX_RESTRICT_VAR dst, const Src& REX_RESTRICT_VAR src) {
  static_assert(sizeof(Dst) == sizeof(Src));
  static_assert(std::is_trivially_copyable_v<Dst>);
  static_assert(std::is_trivially_copyable_v<Src>);
  dst = std::bit_cast<Dst>(src);
}

/** Returns the bits of a trivially copyable object as another type of the same size. */
template <typename Dst, typename Src>
Dst Reinterpret(const Src& REX_RESTRICT_VAR src) {
  Dst dst;
  Reinterpret(dst, src);
  return dst;
}

/**
 * Compares two runs of floats by bit pattern, so a NaN equals itself.
 *
 * @return True when both runs have the same length and the same bits.
 */
inline bool AreBitsEqual(std::span<const f32> a, std::span<const f32> b) {
  return std::ranges::equal(
      a, b, [](f32 x, f32 y) { return std::bit_cast<u32>(x) == std::bit_cast<u32>(y); });
}

}  // namespace rex
