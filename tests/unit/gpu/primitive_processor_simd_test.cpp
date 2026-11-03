/**
 * @file        tests/unit/gpu/primitive_processor_simd_test.cpp
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
#include <memory>
#include <random>
#include <vector>

#include "gpu/xenos/index_conversion.h"

namespace {

constexpr std::array<u32, 6> kCounts = {0, 1, 15, 16, 17, 70};
constexpr u16 kReset16 = 0x1234;
constexpr u32 kMask24 = 0x00FFFFFF;
constexpr u32 kReset32 = 0x00ABCDEF;

std::vector<u16> Indices16(u32 count, bool with_reset, bool with_ffff) {
  std::mt19937 engine(7 + count);
  std::vector<u16> data(count);
  for (u16& value : data) {
    value = static_cast<u16>(engine() % 0x1000);
  }
  if (count && with_reset) {
    data[count - 1] = kReset16;
  }
  if (count > 1 && with_ffff) {
    data[count / 2] = 0xFFFF;
  }
  return data;
}

std::vector<u32> Indices32(u32 count, bool with_reset) {
  std::mt19937 engine(11 + count);
  std::vector<u32> data(count);
  for (u32& value : data) {
    value = engine() & 0x0FFFFFFF;
  }
  if (count && with_reset) {
    data[count - 1] = 0xF1000000 | kReset32;
  }
  return data;
}

template <typename T>
struct AlignedCopy {
  AlignedCopy(const std::vector<T>& values, size_t offset) : storage(values.size() + 64) {
    std::copy(values.begin(), values.end(), Begin(offset));
    start = Begin(offset);
  }
  T* Begin(size_t offset) {
    void* pointer = storage.data();
    size_t space = storage.size() * sizeof(T);
    std::align(16, sizeof(T), pointer, space);
    return static_cast<T*>(pointer) + offset;
  }
  std::vector<T> storage;
  T* start;
};

}  // namespace

REX_TEST_CASE("16 bit reset scans match a plain loop", "[gpu][primitive_processor]") {
  for (const u32 count : kCounts) {
    for (const bool with_reset : {false, true}) {
      for (const bool with_ffff : {false, true}) {
        const auto values = Indices16(count, with_reset, with_ffff);
        bool want_reset = false;
        bool want_ffff = false;
        for (const u16 value : values) {
          want_reset |= value == kReset16;
          want_ffff |= value == 0xFFFF;
        }
        for (size_t offset = 0; offset < 8; ++offset) {
          AlignedCopy<u16> placed(values, offset);
          INFO("count " << count << " offset " << offset);
          CHECK(rex::gpu::IsResetIndexUsed(placed.start, count, kReset16) == want_reset);
          bool used = !want_reset;
          bool ffff = !want_ffff;
          rex::gpu::Get16BitResetIndexUsage(placed.start, count, kReset16, used, ffff);
          CHECK(used == want_reset);
          CHECK(ffff == want_ffff);
        }
      }
    }
  }
}

REX_TEST_CASE("32 bit reset scan masks before comparing", "[gpu][primitive_processor]") {
  for (const u32 count : kCounts) {
    for (const bool with_reset : {false, true}) {
      const auto values = Indices32(count, with_reset);
      bool want = false;
      for (const u32 value : values) {
        want |= (value & kMask24) == kReset32;
      }
      for (size_t offset = 0; offset < 4; ++offset) {
        AlignedCopy<u32> placed(values, offset);
        INFO("count " << count << " offset " << offset);
        CHECK(rex::gpu::IsResetIndexUsed(placed.start, count, kReset32, kMask24) == want);
      }
    }
  }
}

REX_TEST_CASE("16 bit reset replacement matches a plain loop", "[gpu][primitive_processor]") {
  for (const u32 count : kCounts) {
    const auto values = Indices16(count, true, true);
    for (size_t offset = 0; offset < 8; ++offset) {
      for (size_t dest_offset = 0; dest_offset < 8; ++dest_offset) {
        AlignedCopy<u16> source(values, offset);
        AlignedCopy<u16> dest16(std::vector<u16>(count), dest_offset);
        AlignedCopy<u32> dest32(std::vector<u32>(count), dest_offset % 4);
        rex::gpu::ReplaceResetIndex16To16(dest16.start, source.start, count, kReset16);
        rex::gpu::ReplaceResetIndex16To32(dest32.start, source.start, count, kReset16);
        INFO("count " << count << " source " << offset << " dest " << dest_offset);
        for (u32 i = 0; i < count; ++i) {
          const bool reset = values[i] == kReset16;
          REQUIRE(dest16.start[i] == (reset ? u16{0xFFFF} : values[i]));
          REQUIRE(dest32.start[i] == (reset ? UINT32_MAX : u32{values[i]}));
        }
      }
    }
  }
}

namespace {

template <rex::xenos::Endian HostSwap>
void CheckReplaceResetIndex32To24() {
  for (const u32 count : kCounts) {
    const auto values = Indices32(count, true);
    for (size_t offset = 0; offset < 4; ++offset) {
      for (size_t dest_offset = 0; dest_offset < 4; ++dest_offset) {
        AlignedCopy<u32> source(values, offset);
        AlignedCopy<u32> dest(std::vector<u32>(count), dest_offset);
        rex::gpu::ReplaceResetIndex32To24<HostSwap>(dest.start, source.start, count, kReset32,
                                                    kMask24);
        INFO("count " << count << " source " << offset << " dest " << dest_offset);
        for (u32 i = 0; i < count; ++i) {
          const u32 index = values[i] & kMask24;
          const u32 want = index != kReset32 ? rex::xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
          REQUIRE(dest.start[i] == want);
        }
      }
    }
  }
}

}  // namespace

REX_TEST_CASE("32 bit reset replacement matches a plain loop for every host swap",
              "[gpu][primitive_processor]") {
  CheckReplaceResetIndex32To24<rex::xenos::Endian::kNone>();
  CheckReplaceResetIndex32To24<rex::xenos::Endian::k8in16>();
  CheckReplaceResetIndex32To24<rex::xenos::Endian::k8in32>();
  CheckReplaceResetIndex32To24<rex::xenos::Endian::k16in32>();
}
