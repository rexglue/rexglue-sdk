/**
 * @file        tests/unit/core/bit_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <thread>
#include <vector>

#include <rex/bit.h>
#include <rex/types.h>

REX_TEST_CASE("BitMap copies every source word", "[core][bit]") {
  rex::u64 words[3] = {0x1111111111111111, 0x2222222222222222, 0x3333333333333333};
  rex::BitMap map(words, sizeof(words) * 8);
  REQUIRE(map.GetData() == std::vector<rex::u64>{words[0], words[1], words[2]});
}

REX_TEST_CASE("BitMap hands each bit to 1 of 4 racing threads", "[core][bit]") {
  constexpr size_t kBits = 256;
  rex::BitMap map(kBits);
  std::atomic<int> claimed{0};
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&map, &claimed] {
      for (size_t i = 0; i < kBits / 4; ++i) {
        claimed.fetch_add(map.Acquire() != static_cast<size_t>(-1));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(claimed.load() == static_cast<int>(kBits));
  CHECK(map.Acquire() == static_cast<size_t>(-1));
  map.Release(5);
  CHECK(map.Acquire() == 5);
}

template <typename Block>
void CheckSetRange(size_t first, size_t length) {
  constexpr size_t kBlockBits = sizeof(Block) * 8;
  constexpr Block kOutside = static_cast<Block>(0x5555555555555555ull);
  std::vector<Block> bits(8, kOutside);
  rex::SetRange(bits.data(), first, length);
  for (size_t bit = 0; bit < bits.size() * kBlockBits; ++bit) {
    const bool in_range = bit >= first && bit < first + length;
    const bool is_set = (bits[bit / kBlockBits] >> (bit % kBlockBits)) & 1;
    const bool was_set = (kOutside >> (bit % kBlockBits)) & 1;
    CHECK(is_set == (in_range || was_set));
  }
}

REX_TEST_CASE("SetRange sets every bit of a range spanning 3 or more blocks", "[core][bit]") {
  CheckSetRange<rex::u32>(5, 3 * 32 + 7);
  CheckSetRange<rex::u32>(0, 5 * 32);
  CheckSetRange<rex::u32>(31, 4 * 32 + 2);
  CheckSetRange<rex::u64>(5, 3 * 64 + 7);
  CheckSetRange<rex::u64>(0, 5 * 64);
  CheckSetRange<rex::u64>(63, 4 * 64 + 2);
}

REX_TEST_CASE("AppendVarargs formats every argument", "[core][string]") {
  REQUIRE(Format("%s-%d-%s", "alpha", 42, "beta") == "alpha-42-beta");
}

REX_TEST_CASE("utf8_upper_ascii uppercases ASCII letters", "[core][string]") {
  REQUIRE(rex::utf8_upper_ascii("abc XYZ 1") == "ABC XYZ 1");
}

REX_TEST_CASE("SetRange sets every bit of a range spanning 3 or more blocks", "[core][bit]") {
  CheckSetRange<rex::u32>(5, 3 * 32 + 7);
  CheckSetRange<rex::u32>(0, 5 * 32);
  CheckSetRange<rex::u32>(31, 4 * 32 + 2);
  CheckSetRange<rex::u64>(5, 3 * 64 + 7);
  CheckSetRange<rex::u64>(0, 5 * 64);
  CheckSetRange<rex::u64>(63, 4 * 64 + 2);
}
