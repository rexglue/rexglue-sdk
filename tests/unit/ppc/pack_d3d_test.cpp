/**
 * @file        tests/unit/ppc/pack_d3d_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstddef>
#include <utility>

#include "rex_test.h"

#include <rex/ppc/context.h>
#include <rex/ppc/vmx.h>

#include "pack_d3d_cases.h"

namespace {

using PackFunction = u8 (*)(rex::ppc::VRegister&, const rex::ppc::VRegister&);

template <size_t Index>
constexpr PackFunction kPackEntry =
    &rex::ppc::simd::PackD3D<Index / 16, (Index / 4) % 4, Index % 4>;

template <size_t... Index>
constexpr std::array<PackFunction, sizeof...(Index)> MakePackTable(std::index_sequence<Index...>) {
  return {kPackEntry<Index>...};
}

constexpr auto kPackTable = MakePackTable(std::make_index_sequence<size_t{7} * 4 * 4>{});

PackFunction FunctionFor(const PackD3DResult& record) {
  return kPackTable[static_cast<size_t>(record.type) * 16 + static_cast<size_t>(record.mask) * 4 +
                    record.shift];
}

rex::ppc::VRegister MakeRegister(const std::array<u32, 4>& words) {
  rex::ppc::VRegister reg{};
  for (size_t i = 0; i < 4; ++i) {
    reg.u32[3 - i] = words[i];
  }
  return reg;
}

std::array<u32, 4> WordsOf(const rex::ppc::VRegister& reg) {
  std::array<u32, 4> words{};
  for (size_t i = 0; i < 4; ++i) {
    words[i] = reg.u32[3 - i];
  }
  return words;
}

void CheckCases(bool (*selected)(const PackD3DResult&)) {
  size_t checked = 0;
  for (const PackD3DResult& record : kPackD3DResults) {
    if (!selected(record)) {
      continue;
    }
    ++checked;
    rex::ppc::VRegister dest = MakeRegister(kPackD3DInitial);
    const rex::ppc::VRegister source = MakeRegister(record.source);
    const u8 saturated = FunctionFor(record)(dest, source);
    const std::array<u32, 4> got = WordsOf(dest);
    CAPTURE(record.type, record.mask, record.shift, record.source[0], record.source[1],
            record.source[2], record.source[3]);
    CHECK(got == record.expected);
    CHECK(saturated == record.saturated);
  }
  CHECK(checked > 0);
}

}  // namespace

REX_TEST_CASE("PackD3D never writes outside the destination for types 4 and 5 at shift 3",
              "[ppc][pack_d3d]") {
  constexpr u32 kGuard = 0x5A5A5A5Au;
  const auto check = [](PackFunction pack) {
    std::array<rex::ppc::VRegister, 3> regs{};
    for (rex::ppc::VRegister& reg : regs) {
      for (u32& word : reg.u32) {
        word = kGuard;
      }
    }
    const rex::ppc::VRegister source =
        MakeRegister({0x40400001, 0x40400002, 0x40400003, 0x40400004});
    static_cast<void>(pack(regs[1], source));
    for (const u32 word : regs[0].u32) {
      CHECK(word == kGuard);
    }
    for (const u32 word : regs[2].u32) {
      CHECK(word == kGuard);
    }
  };
  check(&rex::ppc::simd::PackD3D<4, 0, 3>);
  check(&rex::ppc::simd::PackD3D<4, 1, 3>);
  check(&rex::ppc::simd::PackD3D<4, 2, 3>);
  check(&rex::ppc::simd::PackD3D<4, 3, 3>);
  check(&rex::ppc::simd::PackD3D<5, 0, 3>);
  check(&rex::ppc::simd::PackD3D<5, 1, 3>);
  check(&rex::ppc::simd::PackD3D<5, 2, 3>);
  check(&rex::ppc::simd::PackD3D<5, 3, 3>);
}

REX_TEST_CASE("PackD3D matches the captured results of types 4 and 5 at shift 3",
              "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 0; });
}

REX_TEST_CASE("PackD3D matches the captured results of the 4_20_20_20 format", "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 1; });
}

REX_TEST_CASE("PackD3D writes only the high word for mask 3 at shift 3", "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 2; });
}

REX_TEST_CASE("PackD3D zero fills the second word of a 1 word type for masks 2 and 3",
              "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 3; });
}

REX_TEST_CASE("PackD3D writes only the low word of a 2 word type for masks 0 and 1",
              "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 4; });
}

REX_TEST_CASE("PackD3D packs every NaN as the field minimum", "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 5; });
}

REX_TEST_CASE("PackD3D flushes a denormal float16 result to a signed zero", "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 6; });
}

REX_TEST_CASE("PackD3D clamps to the hardware range of each field", "[ppc][pack_d3d]") {
  CheckCases([](const PackD3DResult& record) { return record.group == 7; });
}

REX_TEST_CASE("PackD3D reads the whole source before it writes the destination",
              "[ppc][pack_d3d]") {
  for (const PackD3DResult& record : kPackD3DResults) {
    rex::ppc::VRegister separate = MakeRegister(kPackD3DInitial);
    const rex::ppc::VRegister source = MakeRegister(record.source);
    static_cast<void>(FunctionFor(record)(separate, source));
    rex::ppc::VRegister shared = source;
    const u8 saturated = FunctionFor(record)(shared, shared);
    CAPTURE(record.type, record.mask, record.shift);
    for (size_t word = 0; word < 4; ++word) {
      const u32 expected = record.expected[word] == kPackD3DInitial[word] ? record.source[word]
                                                                          : record.expected[word];
      CHECK(WordsOf(shared)[word] == expected);
    }
    CHECK(saturated == record.saturated);
  }
}

REX_TEST_CASE("PackD3D packs the byte format as the lane description says", "[ppc][pack_d3d]") {
  u32 state = 0x2545F491u;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  constexpr auto& kLayout = rex::ppc::simd::detail::kPackLayouts[0];
  for (u32 i = 0; i < 200000; ++i) {
    std::array<u32, 4> words{};
    for (u32& word : words) {
      const u32 pick = next() % 8;
      word = pick == 0   ? 0x40400000u + next() % 300
             : pick == 1 ? 0x40400000u - next() % 300
             : pick == 2 ? (next() | 0x7F800000u)
                         : next();
    }
    rex::ppc::VRegister source = MakeRegister(words);
    u8 saturated = 0;
    u64 packed = 0;
    for (u32 lane = 0; lane < 4; ++lane) {
      packed |= rex::ppc::simd::detail::PackInteger(words[lane],
                                                    rex::ppc::simd::detail::kPackD3DRanges[0][lane],
                                                    kLayout.lanes[lane], saturated)
                << kLayout.lanes[lane].shift;
    }
    rex::ppc::VRegister dest{};
    const u8 got = rex::ppc::simd::PackD3D<0, 0, 0>(dest, source);
    CAPTURE(words[0], words[1], words[2], words[3]);
    REQUIRE(dest.u32[0] == static_cast<u32>(packed));
    REQUIRE(got == saturated);
  }
}

REX_TEST_CASE("PackD3D places the byte of element 0 in bits 16 to 23", "[ppc][pack_d3d]") {
  rex::ppc::VRegister dest{};
  const rex::ppc::VRegister source = MakeRegister({0x40400001, 0x40400002, 0x40400003, 0x40400004});
  CHECK(rex::ppc::simd::PackD3D<0, 0, 0>(dest, source) == 0);
  CHECK(dest.u32[0] == 0x04010203u);
}
