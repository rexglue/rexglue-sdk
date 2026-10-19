/**
 * @file        tests/unit/gpu/texture_info_test.cpp
 * @brief       Texture descriptions compare by field
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <cstddef>
#include <new>

#include <rex/types.h>
#include <rex/xenos/texture_info.h>

namespace {

using rex::xenos::TextureInfo;

struct FilledStorage {
  explicit FilledStorage(std::byte fill) {
    bytes.fill(fill);
    info = new (bytes.data()) TextureInfo;
    info->format = rex::xenos::TextureFormat::k_8_8_8_8;
    info->endianness = rex::xenos::Endian::k8in32;
    info->dimension = rex::xenos::DataDimension::k2DOrStacked;
    info->width_minus_1 = 63;
    info->height_minus_1 = 31;
    info->depth_or_array_size_minus_1 = 0;
    info->pitch = 64;
    info->mip_min_level = 0;
    info->mip_max_level = 6;
    info->is_stacked = false;
    info->is_tiled = true;
    info->has_packed_mips = true;
    info->memory = {0x1000, 0x2000, 0x3000, 0x400};
    info->extent = {64, 32, 64, 32, 64, 32, 1};
  }

  alignas(TextureInfo) std::array<std::byte, sizeof(TextureInfo)> bytes;
  TextureInfo* info;
};

}  // namespace

REX_TEST_CASE("texture descriptions equal in every field compare equal", "[gpu]") {
  const FilledStorage zeros(std::byte{0x00});
  const FilledStorage ones(std::byte{0xFF});
  CHECK(*zeros.info == *ones.info);
}

REX_TEST_CASE("texture descriptions differing in 1 field compare unequal", "[gpu]") {
  const FilledStorage first(std::byte{0x00});
  FilledStorage second(std::byte{0x00});
  second.info->extent.depth = 2;
  CHECK_FALSE(*first.info == *second.info);
}

REX_TEST_CASE("texture descriptions equal in every field hash equal", "[gpu]") {
  const FilledStorage zeros(std::byte{0x00});
  const FilledStorage ones(std::byte{0xFF});
  CHECK(zeros.info->Hash() == ones.info->Hash());
}

REX_TEST_CASE("texture descriptions differing in 1 field hash differently", "[gpu]") {
  const FilledStorage first(std::byte{0x00});
  FilledStorage second(std::byte{0x00});
  second.info->extent.depth = 2;
  CHECK(first.info->Hash() != second.info->Hash());
}
