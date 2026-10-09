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

#include <bit>
#include <cmath>

#include "gpu/xenos/host/graphics_util.h"

#include <rex/types.h>

namespace rex {
namespace gpu {

i32 FloatToD3D11Fixed16p8(float f32) {
  if (!(std::abs(f32) >= 1.0f / 512.0f)) {
    return 0;
  }
  if (f32 >= 32768.0f - 1.0f / 256.0f) {
    return (1 << 23) - 1;
  }
  if (f32 <= -32768.0f) {
    return -32768 * 256;
  }
  u32 f32_bits = std::bit_cast<u32>(f32);
  union {
    i32 s;
    u32 u;
  } result;
  result.u = (f32_bits & ((1 << 23) - 1)) | (1 << 23);
  if ((f32_bits >> 31) != 0) {
    result.s = -result.s;
  }
  i32 exponent = static_cast<i32>((f32_bits >> 23) & 255) - 127;
  u32 extra_bits = static_cast<u32>(15 - exponent);
  if (extra_bits) {
    result.u += (1 << (extra_bits - 1)) - 1 + ((result.u >> extra_bits) & 1);
    result.s >>= extra_bits;
  }
  return result.s;
}

}  // namespace gpu
}  // namespace rex
