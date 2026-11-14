/**
 * @file        tests/unit/gpu/texture_layout_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/types.h>
#include <rex/xenos/texture.h>
#include <rex/xenos/fetch.h>

namespace {

constexpr rex::u32 kPitch = 64;
constexpr rex::u32 kHeight = 64;
constexpr rex::u32 kBytesPerBlockLog2 = 2;

}  // namespace

REX_TEST_CASE("The 3D lower bound of every layer in a tile is the first layer of the tile",
              "[gpu][texture]") {
  const rex::u32 first =
      rex::xenos::GetTiledAddressLowerBound3D(0, 0, 0, kPitch, kHeight, kBytesPerBlockLog2);
  for (rex::u32 front = 1; front < rex::xenos::kTextureTileDepth; ++front) {
    CHECK(rex::xenos::GetTiledAddressLowerBound3D(0, 0, front, kPitch, kHeight,
                                                  kBytesPerBlockLog2) == first);
  }
}

REX_TEST_CASE("The 3D lower bound moves to the next tile at the tile depth", "[gpu][texture]") {
  const rex::u32 first =
      rex::xenos::GetTiledAddressLowerBound3D(0, 0, 0, kPitch, kHeight, kBytesPerBlockLog2);
  const rex::u32 next = rex::xenos::GetTiledAddressLowerBound3D(
      0, 0, rex::xenos::kTextureTileDepth, kPitch, kHeight, kBytesPerBlockLog2);
  constexpr rex::u32 kTileBlocks = rex::xenos::kTextureTileWidthHeight *
                                   rex::xenos::kTextureTileWidthHeight *
                                   rex::xenos::kTextureTileDepth;
  constexpr rex::u32 kTilesPerSlab = (kPitch / rex::xenos::kTextureTileWidthHeight) *
                                     (kHeight / rex::xenos::kTextureTileWidthHeight);
  constexpr rex::u32 kSlabBytes = kTilesPerSlab * (kTileBlocks << kBytesPerBlockLog2);
  constexpr rex::u32 kBankSwizzleBytes = 0x880;
  CHECK(first == 0);
  CHECK(next == kSlabBytes + kBankSwizzleBytes);
}
