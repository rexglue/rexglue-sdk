/**
 * @file        bit.h
 * @brief       Atomic free list bitmap and bit range scan and fill helpers
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

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
  const std::vector<u64> data() const { return data_; }

  /** @return The backing words. */
  std::vector<u64>& data() { return data_; }

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
    return std::make_pair(size_t(first), size_t(0));
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
    return std::make_pair(range_start, last + size_t(1) - range_start);
  }
  return std::make_pair(first + length, size_t(0));
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

}  // namespace rex
