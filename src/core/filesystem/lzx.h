/**
 * @file        core/filesystem/lzx.h
 * @brief       LZX decompression and LZX delta patches for XEX images
 *
 * @copyright   Copyright 2022 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <span>

#include <rex/types.h>

namespace rex::filesystem::detail {

/**
 * Decompresses an LZX stream.
 *
 * @param input The compressed stream.
 * @param output The buffer that receives exactly output.size() bytes.
 * @param window_size The LZX window size in bytes, a power of 2.
 * @param window_data Reference data placed at the end of the window, empty for none.
 * @return 0 on success, otherwise the libmspack error code.
 */
int LZXDecompress(std::span<const u8> input, std::span<u8> output, u32 window_size,
                  std::span<const u8> window_data);

/**
 * Applies a run of XEX delta patch records to a buffer in place.
 *
 * @param patch The patch records.
 * @param window_size The LZX window size in bytes.
 * @param image The buffer the records address.
 * @return 0 on success, 1 when a record reaches outside a buffer, otherwise the libmspack error
 * code.
 */
int LZXApplyDelta(std::span<const u8> patch, u32 window_size, std::span<u8> image);

}  // namespace rex::filesystem::detail
