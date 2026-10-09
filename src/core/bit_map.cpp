/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/math.h>
#include <rex/platform/atomic.h>

namespace rex {

BitMap::BitMap() = default;

BitMap::BitMap(size_t size_bits) {
  Resize(size_bits);
}

BitMap::BitMap(u64* data, size_t size_bits) {
  REX_ASSERT(size_bits % kDataSizeBits == 0);

  data_.resize(size_bits / kDataSizeBits);
  std::memcpy(data_.data(), data, data_.size() * sizeof(u64));
}

size_t BitMap::Acquire() {
  for (size_t i = 0; i < data_.size(); i++) {
    u64 entry = 0;
    u64 new_entry = 0;
    i64 acquired_idx = -1;

    do {
      entry = data_[i];
      u8 index = lzcnt(entry);
      if (index == kDataSizeBits) {
        acquired_idx = -1;
        break;
      }

      u64 bit = 1ull << (kDataSizeBits - index - 1);
      new_entry = entry & ~bit;
      REX_ASSERT((entry & bit) != 0);

      acquired_idx = index;
    } while (!platform::atomic_cas(entry, new_entry, &data_[i]));

    if (acquired_idx != -1) {
      return (i * kDataSizeBits) + acquired_idx;
    }
  }

  return -1;
}

void BitMap::Release(size_t index) {
  auto slot = index / kDataSizeBits;
  index -= slot * kDataSizeBits;

  u64 bit = 1ull << (kDataSizeBits - index - 1);

  u64 entry = 0;
  u64 new_entry = 0;
  do {
    entry = data_[slot];
    REX_ASSERT((entry & bit) == 0);

    new_entry = entry | bit;
  } while (!platform::atomic_cas(entry, new_entry, &data_[slot]));
}

void BitMap::Resize(size_t new_size_bits) {
  auto old_size = data_.size();
  REX_ASSERT(new_size_bits % kDataSizeBits == 0);
  data_.resize(new_size_bits / kDataSizeBits);

  if (data_.size() > old_size) {
    for (size_t i = old_size; i < data_.size(); i++) {
      data_[i] = -1;
    }
  }
}

void BitMap::Reset() {
  std::ranges::fill(data_, -1);
}

}  // namespace rex