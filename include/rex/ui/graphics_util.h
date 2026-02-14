#pragma once
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

#include <cstdint>

namespace rex {
namespace ui {

// For estimating coverage extents from vertices. This may give results that are
// different than what the GPU will actually draw (this is the reference
// conversion with 1/2 ULP accuracy, but Direct3D 11 permits 0.6 ULP tolerance
// in floating point to fixed point conversion), but is enough to tie-break
// vertices at pixel centers (due to the half-pixel offset applied to integer
// coordinates incorrectly, for instance) with some error tolerance near 0.5,
// for use with the top-left rasterization rule later.
int32_t FloatToD3D11Fixed16p8(float f32);

}  // namespace ui
}  // namespace rex
