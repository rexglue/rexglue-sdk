/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

namespace rex {
namespace gpu {

/**
 * Converts a float to Direct3D 11 16p8 fixed point for estimating coverage extents from vertices.
 *
 * Follows the Direct3D 11.3 functional spec FLOAT to fixed point conversion with 1/2 ULP accuracy,
 * while Direct3D 11 permits a 0.6 ULP tolerance, so results may differ from what the GPU draws.
 * Good enough to tie break vertices at pixel centers near 0.5 for the top left rasterization rule.
 * NaN and magnitudes below 1/512 give 0, and values saturate at the 24 bit signed range. The
 * float mantissa with its hidden bit is negated for negative inputs, then rounded to nearest even
 * by 15 minus the unbiased exponent bits before an arithmetic shift right.
 *
 * @param f32 Value to convert.
 * @return The fixed point value with 8 fractional bits.
 */
int32_t FloatToD3D11Fixed16p8(float f32);

}  // namespace gpu
}  // namespace rex
