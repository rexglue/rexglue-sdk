/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_FLAT_INDEX_MAP_H_
#define XENIA_GPU_METAL_METAL_FLAT_INDEX_MAP_H_

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rex {
namespace graphics {
namespace metal {

// uint64 key -> uint32 index, open addressing with linear probing. Insert and
// lookup only; Clear keeps the storage, so a table rebuilt every frame stops
// allocating once it has grown to the working set (std::unordered_map
// allocated a node per insert: ~4% of the command-processor thread).
class MetalFlatIndexMap {
 public:
  static constexpr uint32_t kEmpty = UINT32_MAX;

  void Clear() {
    if (size_) {
      std::fill(values_.begin(), values_.end(), kEmpty);
      size_ = 0;
    }
  }
  void Reserve(size_t count) {
    size_t want = 16;
    while (want < count * 2) {
      want <<= 1;
    }
    if (want > values_.size()) {
      Rehash(want);
    }
  }
  // Returns the stored index, or nullptr.
  uint32_t* Find(uint64_t key) {
    if (!size_) {
      return nullptr;
    }
    const size_t mask = values_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      if (values_[i] == kEmpty) {
        return nullptr;
      }
      if (keys_[i] == key) {
        return &values_[i];
      }
    }
  }
  // overwrite=false keeps an existing value (std::unordered_map::emplace);
  // overwrite=true replaces it (operator[]).
  void Insert(uint64_t key, uint32_t value, bool overwrite) {
    if ((size_ + 1) * 2 > values_.size()) {
      Rehash(values_.empty() ? 16 : values_.size() * 2);
    }
    const size_t mask = values_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      if (values_[i] == kEmpty) {
        keys_[i] = key;
        values_[i] = value;
        ++size_;
        return;
      }
      if (keys_[i] == key) {
        if (overwrite) {
          values_[i] = value;
        }
        return;
      }
    }
  }

 private:
  static size_t Hash(uint64_t key) {
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDull;
    key ^= key >> 33;
    return size_t(key);
  }
  void Rehash(size_t capacity) {
    std::vector<uint64_t> old_keys = std::move(keys_);
    std::vector<uint32_t> old_values = std::move(values_);
    keys_.assign(capacity, 0);
    values_.assign(capacity, kEmpty);
    size_ = 0;
    for (size_t i = 0; i < old_values.size(); ++i) {
      if (old_values[i] != kEmpty) {
        Insert(old_keys[i], old_values[i], true);
      }
    }
  }

  std::vector<uint64_t> keys_;
  std::vector<uint32_t> values_;
  size_t size_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_FLAT_INDEX_MAP_H_
