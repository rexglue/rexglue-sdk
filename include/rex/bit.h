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

#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/macros.h>
#include <rex/math.h>
#include <rex/types.h>

namespace rex {

/** Bitmap of free and used entries where a set bit marks a free entry. */
class REX_API BitMap {
 public:
  /** Creates an empty bitmap. */
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
  size_t Acquire();

  /**
   * Atomically marks an entry as free. Thread safe.
   *
   * @param index The entry to release.
   */
  void Release(size_t index);

  /**
   * Resizes the bitmap. New entries start free.
   *
   * @param new_size_bits Number of entries, a multiple of 64.
   */
  void Resize(size_t new_size_bits);

  /** Marks every entry free. */
  void Reset();

  /** @return A copy of the backing words. */
  const std::vector<u64> GetData() const { return data_; }

  /** @return The backing words. */
  std::vector<u64>& GetData() { return data_; }

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
    std::memset(bits + block_first + 1, CHAR_MAX, (block_last - (block_first + 1)) * sizeof(Block));
  }
  bits[block_last] |= set_last;
}

/**
 * Opts an enum into the flag operators. Specialize to true next to the enum.
 *
 * @tparam E The enum type.
 *
 * @code
 * template <>
 * inline constexpr bool rex::is_flags_enum<rex::filesystem::MountFlags> = true;
 * @endcode
 */
template <typename E>
inline constexpr bool is_flags_enum = false;

/**
 * An enum that opted into the flag operators through is_flags_enum.
 *
 * @tparam E The enum type.
 */
template <typename E>
concept FlagsEnum = std::is_enum_v<E> && is_flags_enum<E>;

/**
 * Combines 2 flag sets.
 *
 * @tparam E The flag enum.
 * @param lhs The first set.
 * @param rhs The second set.
 * @return The bitwise or of both sets.
 */
template <FlagsEnum E>
[[nodiscard]] constexpr E operator|(E lhs, E rhs) {
  return static_cast<E>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

/**
 * Intersects 2 flag sets.
 *
 * @tparam E The flag enum.
 * @param lhs The first set.
 * @param rhs The second set.
 * @return The bitwise and of both sets.
 */
template <FlagsEnum E>
[[nodiscard]] constexpr E operator&(E lhs, E rhs) {
  return static_cast<E>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

/**
 * Toggles the flags of 1 set by another.
 *
 * @tparam E The flag enum.
 * @param lhs The first set.
 * @param rhs The second set.
 * @return The bitwise exclusive or of both sets.
 */
template <FlagsEnum E>
[[nodiscard]] constexpr E operator^(E lhs, E rhs) {
  return static_cast<E>(std::to_underlying(lhs) ^ std::to_underlying(rhs));
}

/**
 * Inverts a flag set.
 *
 * @tparam E The flag enum.
 * @param value The set.
 * @return The bitwise complement of the set.
 */
template <FlagsEnum E>
[[nodiscard]] constexpr E operator~(E value) {
  return static_cast<E>(~std::to_underlying(value));
}

/**
 * Adds flags to a set in place.
 *
 * @tparam E The flag enum.
 * @param lhs The set to change.
 * @param rhs The flags to add.
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
 * @tparam E The flag enum.
 * @param lhs The set to change.
 * @param rhs The flags to keep.
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
 * @tparam E The flag enum.
 * @param lhs The set to change.
 * @param rhs The flags to toggle.
 * @return lhs.
 */
template <FlagsEnum E>
constexpr E& operator^=(E& lhs, E rhs) {
  lhs = lhs ^ rhs;
  return lhs;
}

/**
 * Reports whether a set holds every flag of another set.
 *
 * @tparam E The flag enum.
 * @param value The set to test.
 * @param flags The flags that must be present.
 * @return True when every flag of flags is set in value.
 */
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

/**
 * Copies the bits of 1 trivially copyable object into another of the same size.
 *
 * @tparam Dst Destination type.
 * @tparam Src Source type, the same size as Dst.
 * @param dst Object that receives the bits.
 * @param src Object the bits come from.
 */
template <typename Dst, typename Src>
void Reinterpret(Dst& REX_RESTRICT_VAR dst, const Src& REX_RESTRICT_VAR src) {
  static_assert(sizeof(Dst) == sizeof(Src));
  static_assert(std::is_trivially_copyable_v<Dst>);
  static_assert(std::is_trivially_copyable_v<Src>);
  dst = std::bit_cast<Dst>(src);
}

/**
 * Returns the bits of a trivially copyable object as another type of the same size.
 *
 * @tparam Dst Result type.
 * @tparam Src Source type, the same size as Dst.
 * @param src Object the bits come from.
 * @return A Dst holding the bits of src.
 */
template <typename Dst, typename Src>
Dst Reinterpret(const Src& REX_RESTRICT_VAR src) {
  Dst dst;
  Reinterpret(dst, src);
  return dst;
}

}  // namespace rex
