/**
 * @file        runtime/lzx.h
 * @brief       LZX decompression interface for XEX loading
 *
 * @copyright   Copyright 2022 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex {
struct xex2_delta_patch;
}  // namespace rex

int lzx_decompress(const void* lzx_data, size_t lzx_len, void* dest, size_t dest_len,
                   uint32_t window_size, void* window_data, size_t window_data_len);

int lzxdelta_apply_patch(rex::xex2_delta_patch* patch, size_t patch_len, uint32_t window_size,
                         void* dest);
