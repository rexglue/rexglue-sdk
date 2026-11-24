/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_EDRAM_LAYOUT_H_
#define XENIA_GPU_EDRAM_LAYOUT_H_

#include <cstdint>

#include "rex/graphics/xenos.h"

// CPU-side canonical Xenos EDRAM layout and word helpers. The Metal equivalents
// live together in metal/metal_edram_msl.h and are checked against these
// by the L1 conformance suite. Keep all snapshot, diagnostic, and canonical-
// buffer code on these helpers rather than carrying local addressing formulas.

namespace rex {
namespace graphics {

struct EdramMsaaShifts {
  uint32_t x;
  uint32_t y;
};

constexpr EdramMsaaShifts GetEdramMsaaShifts(uint32_t msaa_samples) {
  return {msaa_samples >= uint32_t(xenos::MSAASamples::k4X) ? 1u : 0u,
          msaa_samples >= uint32_t(xenos::MSAASamples::k2X) ? 1u : 0u};
}

constexpr uint32_t EdramDepthSwapX(uint32_t tile_sample_x,
                                   uint32_t tile_width_samples) {
  uint32_t half_width = tile_width_samples >> 1u;
  return tile_sample_x >= half_width ? tile_sample_x - half_width
                                     : tile_sample_x + half_width;
}

// Byte offset of one sample in the raw 10 MiB tile-major EDRAM image.
// Mirrors shaders/edram.xesli XeEdramOffsetBytes at resolution_scale=(1,1),
// wrap=false.
constexpr uint32_t XeEdramSnapshotOffsetBytes(
    uint32_t pixel_x, uint32_t pixel_y, uint32_t base_tiles,
    uint32_t pitch_tiles, uint32_t msaa_samples, bool is_depth,
    uint32_t format_ints_log2, uint32_t pixel_sample_index) {
  EdramMsaaShifts shifts = GetEdramMsaaShifts(msaa_samples);
  uint32_t sample_x = (pixel_x << shifts.x) +
                      ((pixel_sample_index >> 1u) & 1u);
  uint32_t sample_y =
      (pixel_y << shifts.y) + (pixel_sample_index & 1u);
  uint32_t tile_width_samples =
      xenos::kEDRAMTileWidthSamples >> format_ints_log2;
  uint32_t tile_x = sample_x / tile_width_samples;
  uint32_t tile_y = sample_y / xenos::kEDRAMTileHeightSamples;
  base_tiles += tile_y * pitch_tiles + tile_x;
  sample_x -= tile_x * tile_width_samples;
  sample_y -= tile_y * xenos::kEDRAMTileHeightSamples;
  if (is_depth) {
    sample_x = EdramDepthSwapX(sample_x, tile_width_samples);
  }
  uint32_t word_index =
      base_tiles *
          (xenos::kEDRAMTileWidthSamples * xenos::kEDRAMTileHeightSamples) +
      ((sample_y * tile_width_samples + sample_x) << format_ints_log2);
  return word_index * sizeof(uint32_t);
}

constexpr uint32_t XeEdramPitchTiles(uint32_t surface_pitch_pixels,
                                     uint32_t msaa_samples,
                                     uint32_t format_ints_log2 = 0u) {
  EdramMsaaShifts shifts = GetEdramMsaaShifts(msaa_samples);
  uint32_t width_samples = surface_pitch_pixels << shifts.x;
  uint32_t tile_width_samples =
      xenos::kEDRAMTileWidthSamples >> format_ints_log2;
  return (width_samples + tile_width_samples - 1u) / tile_width_samples;
}

// Canonical 32bpp 4x sample plane of a tile-local raw dword. This is the same
// parity classification used by XEDH v2 diagnostics.
constexpr uint32_t EdramSamplePlaneForTileWord(uint32_t tile_word) {
  uint32_t x = tile_word % xenos::kEDRAMTileWidthSamples;
  uint32_t y = tile_word / xenos::kEDRAMTileWidthSamples;
  return ((x & 1u) << 1u) | (y & 1u);
}

constexpr uint32_t PackEdramColor8888(uint32_t red, uint32_t green,
                                      uint32_t blue, uint32_t alpha) {
  return (red & 0xFFu) | ((green & 0xFFu) << 8u) |
         ((blue & 0xFFu) << 16u) | ((alpha & 0xFFu) << 24u);
}

constexpr uint32_t GetEdramColor8888Channel(uint32_t word,
                                            uint32_t channel) {
  return (word >> ((channel & 3u) * 8u)) & 0xFFu;
}

constexpr uint32_t PackEdramDepthStencil(uint32_t depth24,
                                         uint32_t stencil) {
  return ((depth24 & 0xFFFFFFu) << 8u) | (stencil & 0xFFu);
}

constexpr uint32_t GetEdramDepth24(uint32_t word) { return word >> 8u; }
constexpr uint32_t GetEdramStencil(uint32_t word) { return word & 0xFFu; }
constexpr uint32_t GetEdramStencilBitMask(uint32_t bit_index) {
  return 1u << (bit_index & 7u);
}

// CPU mirror of the canonical Metal SurfaceParams constant-buffer ABI.
struct EdramSurfaceParams {
  uint32_t base;
  uint32_t pitch;
  uint32_t msaa;
  // 0=color, 1=D24S8, 2=D24FS8. Any nonzero value applies the depth swap.
  uint32_t is_depth;
  uint32_t format_ints_log2;
  uint32_t words;
};
static_assert(sizeof(EdramSurfaceParams) == 6u * sizeof(uint32_t));

}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_EDRAM_LAYOUT_H_
