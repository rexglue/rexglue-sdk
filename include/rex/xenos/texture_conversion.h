/**
 * @file        xenos/texture_conversion.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/gpu/texture_conversion.h (Ben Vanik, 2014).
 */

#pragma once

#include <cstring>
#include <functional>
#include <memory>

#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/export.h>
#include <rex/types.h>
#include <rex/xenos/texture_info.h>
#include <rex/xenos/xenos.h>

namespace rex::xenos {

/** Copies a block with endian swapping, taking endian, output, input and byte length. */
using CopyBlockCallback = std::function<void(xenos::Endian, void*, const void*, size_t)>;

/**
 * Copies bytes, swapping them as endian specifies.
 *
 * @param endian The swap mode.
 * @param output Destination buffer.
 * @param input Source buffer.
 * @param length Number of bytes to copy.
 */
REX_API void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length);

/**
 * Decodes an 8 byte CTX1 block into 4x4 R8G8 texels.
 *
 * @param endian The swap mode of the source block.
 * @param output Destination for 4 rows of 4 R8G8 texels.
 * @param input Source CTX1 block.
 * @param length Destination row pitch in bytes.
 */
REX_API void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input,
                                    size_t length);

/**
 * Expands a DXT3A block into a DXT3 block with the color half zeroed.
 *
 * @param endian The swap mode of the source block.
 * @param output Destination 16 byte DXT3 block.
 * @param input Source 8 byte DXT3A block.
 * @param length Unused.
 */
REX_API void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input,
                                     size_t length);

/** Copies an untiled block, taking output, input and byte length. */
using UntileCopyBlockCallback = std::function<void(void*, const void*, size_t)>;

/** Region and formats for Untile. */
struct UntileInfo {
  u32 offset_x;                          /**< Left edge of the region in blocks. */
  u32 offset_y;                          /**< Top edge of the region in blocks. */
  u32 width;                             /**< Region width in blocks. */
  u32 height;                            /**< Region height in blocks. */
  u32 input_pitch;                       /**< Tiled source pitch in blocks. */
  u32 output_pitch;                      /**< Linear destination pitch in blocks. */
  const FormatInfo* input_format_info;   /**< Source format. */
  const FormatInfo* output_format_info;  /**< Destination format. */
  UntileCopyBlockCallback copy_callback; /**< Copies each block. */
};

/**
 * Converts a region of a 2D tiled texture to linear layout.
 *
 * @param output_buffer Linear destination.
 * @param input_buffer Tiled source.
 * @param untile_info The region, pitches, formats and block copy callback.
 */
REX_API void Untile(u8* output_buffer, const u8* input_buffer, const UntileInfo* untile_info);

}  // namespace rex::xenos
