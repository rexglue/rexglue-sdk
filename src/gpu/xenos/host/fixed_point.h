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

#include <rex/types.h>

namespace rex {
namespace gpu {

/**
 * Converts a float to Direct3D 11 16p8 fixed point for estimating coverage extents from vertices.
 *
 * NaN and magnitudes below 1/512 give 0, and values saturate at the 24 bit signed range.
 *
 * @param value Number to convert, clamped to the 16.8 range.
 * @return The fixed point value with 8 fractional bits.
 */
i32 FloatToD3D11Fixed16p8(f32 value);

}  // namespace gpu
}  // namespace rex
