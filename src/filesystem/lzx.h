/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
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
 * @param output The buffer that receives exactly output.size() bytes.
 * @param window_size The LZX window size in bytes, a power of 2.
 * @param window_data Reference data placed at the end of the window, empty for none.
 * @return 0 on success, otherwise the libmspack error code.
 */
int LZXDecompress(std::span<const u8> input, std::span<u8> output, u32 window_size,
                  std::span<const u8> window_data);

/**
 * Applies a run of XEX delta patch entries to a buffer in place.
 *
 * @param window_size The LZX window size in bytes.
 * @param image The buffer the entries address.
 * @return 0 on success, 1 when an entry reaches outside a buffer, otherwise the libmspack error
 * code.
 */
int LZXApplyDelta(std::span<const u8> patch, u32 window_size, std::span<u8> image);

}  // namespace rex::filesystem::detail
