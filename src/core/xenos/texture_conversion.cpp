/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <rex/dbg.h>
#include <rex/xenos/texture_conversion.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::xenos {

void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length) {
  switch (endian) {
    case xenos::Endian::k8in16:
      memory::copy_and_swap_16_unaligned(output, input, length / 2);
      break;
    case xenos::Endian::k8in32:
      memory::copy_and_swap_32_unaligned(output, input, length / 4);
      break;
    case xenos::Endian::k16in32:
      memory::copy_and_swap_16_in_32_unaligned(output, input, length);
      break;
    default:
    case xenos::Endian::kNone:
      std::memcpy(output, input, length);
      break;
  }
}

void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input, size_t length) {
  union {
    u8 data[8];
    struct {
      u8 g0, r0, g1, r1;
      u32 xx;
    };
  } block;
  static_assert(sizeof(block) == 8, "CTX1 block mismatch");

  const u32 bytes_per_block = 8;
  CopySwapBlock(endian, block.data, input, bytes_per_block);

  u8 cr[4] = {block.r0, block.r1, static_cast<u8>(2.f / 3.f * block.r0 + 1.f / 3.f * block.r1),
              static_cast<u8>(1.f / 3.f * block.r0 + 2.f / 3.f * block.r1)};
  u8 cg[4] = {block.g0, block.g1, static_cast<u8>(2.f / 3.f * block.g0 + 1.f / 3.f * block.g1),
              static_cast<u8>(1.f / 3.f * block.g0 + 2.f / 3.f * block.g1)};

  auto output_bytes = static_cast<u8*>(output);
  for (u32 oy = 0; oy < 4; ++oy) {
    for (u32 ox = 0; ox < 4; ++ox) {
      u8 xx = (block.xx >> ((ox + oy * 4) * 2)) & 3;
      output_bytes[(static_cast<size_t>(oy) * length) + (static_cast<size_t>(ox) * 2) + 0] = cr[xx];
      output_bytes[(static_cast<size_t>(oy) * length) + (static_cast<size_t>(ox) * 2) + 1] = cg[xx];
    }
  }
}

void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input, size_t length) {
  const u32 bytes_per_block = 16;
  auto output_bytes = static_cast<u8*>(output);
  CopySwapBlock(endian, &output_bytes[0], input, 8);
  std::memset(&output_bytes[8], 0, 8);
}

static u32 TiledOffset2DRow(u32 y, u32 width, u32 log2_bpp) {
  u32 macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  u32 micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) +
         ((y & 1) << 4);
}

static u32 TiledOffset2DColumn(u32 x, u32 y, u32 log2_bpp, u32 base_offset) {
  u32 macro = (x / 32) << (log2_bpp + 7);
  u32 micro = (x & 7) << log2_bpp;
  u32 offset = base_offset + (macro + ((micro & ~0xF) << 1) + (micro & 0xF));
  return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

void Untile(u8* output_buffer, const u8* input_buffer, const UntileInfo* untile_info) {
  REX_PROFILE_SCOPE("gpu");
  assert_not_null(untile_info);
  assert_not_null(untile_info->input_format_info);
  assert_not_null(untile_info->output_format_info);

  u32 input_bytes_per_block = untile_info->input_format_info->bytes_per_block();
  u32 output_bytes_per_block = untile_info->output_format_info->bytes_per_block();
  u32 output_pitch = untile_info->output_pitch * output_bytes_per_block;

  auto log2_bpp =
      (input_bytes_per_block / 4) + ((input_bytes_per_block / 2) >> (input_bytes_per_block / 4));

  u32 output_row_offset = 0;
  for (u32 y = 0; y < untile_info->height; y++) {
    auto input_row_offset =
        TiledOffset2DRow(untile_info->offset_y + y, untile_info->input_pitch, log2_bpp);

    u32 output_offset = output_row_offset;

    for (u32 x = 0; x < untile_info->width; x++) {
      auto input_offset = TiledOffset2DColumn(untile_info->offset_x + x, untile_info->offset_y + y,
                                              log2_bpp, input_row_offset);
      input_offset >>= log2_bpp;

      untile_info->copy_callback(
          &output_buffer[output_offset],
          &input_buffer[static_cast<size_t>(input_offset) * input_bytes_per_block],
          output_bytes_per_block);

      output_offset += output_bytes_per_block;
    }

    output_row_offset += output_pitch;
  }
}

}  // namespace rex::xenos
