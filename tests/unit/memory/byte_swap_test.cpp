/**
 * @file        tests/unit/memory/byte_swap_test.cpp
 * @brief       The copy and swap functions match a plain loop at every count and misalignment
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <vector>

#include <rex/memory/byte_swap.h>
#include <rex/types.h>

namespace {

constexpr size_t kGuard = 32;
constexpr u8 kFill = 0xCD;
constexpr size_t kAlignedBytes = 1600;
constexpr std::array<size_t, 6> kCounts = {0, 1, 15, 16, 17, 70};

using CopyFn = void (*)(void*, const void*, size_t);

std::vector<u8> ReferenceSwap(const std::vector<u8>& source, size_t width, size_t unit) {
  std::vector<u8> out(source.size());
  for (size_t i = 0; i + width <= source.size(); i += width) {
    for (size_t j = 0; j < width; j += unit) {
      for (size_t k = 0; k < unit; ++k) {
        out[i + j + k] = source[i + j + unit - 1 - k];
      }
    }
  }
  return out;
}

std::vector<u8> RandomBytes(size_t size) {
  std::mt19937 engine(0x5EED + static_cast<u32>(size));
  std::vector<u8> bytes(size);
  for (u8& byte : bytes) {
    byte = static_cast<u8>(engine());
  }
  return bytes;
}

void CheckUnaligned(CopyFn copy, size_t width, size_t unit) {
  for (const size_t count : kCounts) {
    const size_t size = count * width;
    const std::vector<u8> source_data = RandomBytes(size);
    const std::vector<u8> expected = ReferenceSwap(source_data, width, unit);
    for (size_t source_offset = 0; source_offset < 16; ++source_offset) {
      for (size_t dest_offset = 0; dest_offset < 16; ++dest_offset) {
        std::vector<u8> source(kGuard + size + kGuard, 0);
        std::copy(source_data.begin(), source_data.end(), source.data() + source_offset);
        std::vector<u8> dest(kGuard + size + kGuard, kFill);
        copy(dest.data() + dest_offset, source.data() + source_offset, count);
        std::vector<u8> want(dest.size(), kFill);
        std::copy(expected.begin(), expected.end(), want.data() + dest_offset);
        INFO("width " << width << " count " << count << " source " << source_offset << " dest "
                      << dest_offset);
        REQUIRE(dest == want);
      }
    }
  }
}

void CheckAligned(CopyFn copy, size_t width, size_t unit) {
  for (const size_t count : kCounts) {
    const size_t size = count * width;
    const std::vector<u8> source_data = RandomBytes(size);
    const std::vector<u8> expected = ReferenceSwap(source_data, width, unit);
    alignas(16) std::array<u8, kAlignedBytes> source{};
    alignas(16) std::array<u8, kAlignedBytes> dest{};
    dest.fill(kFill);
    std::copy(source_data.begin(), source_data.end(), source.data());
    copy(dest.data(), source.data(), count);
    INFO("width " << width << " count " << count);
    REQUIRE(std::equal(expected.begin(), expected.end(), dest.data()));
    REQUIRE(std::all_of(dest.data() + size, dest.data() + dest.size(),
                        [](u8 byte) { return byte == kFill; }));
  }
}

}  // namespace

REX_TEST_CASE("copy_and_swap_16 matches a plain loop", "[memory][byte_swap]") {
  CheckUnaligned(rex::memory::copy_and_swap_16_unaligned, 2, 2);
  CheckAligned(rex::memory::copy_and_swap_16_aligned, 2, 2);
}

REX_TEST_CASE("copy_and_swap_32 matches a plain loop", "[memory][byte_swap]") {
  CheckUnaligned(rex::memory::copy_and_swap_32_unaligned, 4, 4);
  CheckAligned(rex::memory::copy_and_swap_32_aligned, 4, 4);
}

REX_TEST_CASE("copy_and_swap_64 matches a plain loop", "[memory][byte_swap]") {
  CheckUnaligned(rex::memory::copy_and_swap_64_unaligned, 8, 8);
  CheckAligned(rex::memory::copy_and_swap_64_aligned, 8, 8);
}

REX_TEST_CASE("copy_and_swap_16_in_32 swaps the halves of every word", "[memory][byte_swap]") {
  const auto swap_halves = [](const std::vector<u8>& source) {
    std::vector<u8> out = source;
    for (size_t i = 0; i + 4 <= out.size(); i += 4) {
      std::swap(out[i], out[i + 2]);
      std::swap(out[i + 1], out[i + 3]);
    }
    return out;
  };
  for (const size_t count : kCounts) {
    const size_t size = count * 4;
    const std::vector<u8> source_data = RandomBytes(size);
    const std::vector<u8> expected = swap_halves(source_data);
    for (size_t source_offset = 0; source_offset < 16; ++source_offset) {
      for (size_t dest_offset = 0; dest_offset < 16; ++dest_offset) {
        std::vector<u8> source(kGuard + size + kGuard, 0);
        std::copy(source_data.begin(), source_data.end(), source.data() + source_offset);
        std::vector<u8> dest(kGuard + size + kGuard, kFill);
        rex::memory::copy_and_swap_16_in_32_unaligned(dest.data() + dest_offset,
                                                      source.data() + source_offset, count);
        std::vector<u8> want(dest.size(), kFill);
        std::copy(expected.begin(), expected.end(), want.data() + dest_offset);
        INFO("count " << count << " source " << source_offset << " dest " << dest_offset);
        REQUIRE(dest == want);
      }
    }
  }
}

REX_TEST_CASE("The 24 bit helpers load and store 3 byte values", "[memory][byte_swap]") {
  constexpr std::array<u8, 3> kBytes = {0x12, 0x34, 0x56};
  REQUIRE(rex::memory::LoadUint24BE(kBytes.data()) == 0x123456);
  REQUIRE(rex::memory::LoadUint24LE(kBytes.data()) == 0x563412);
  std::array<u8, 3> stored{};
  rex::memory::StoreUint24LE(stored.data(), 0xFF563412);
  REQUIRE(stored == kBytes);
}
