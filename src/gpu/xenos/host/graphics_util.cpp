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

namespace rex {
namespace gpu {

int32_t FloatToD3D11Fixed16p8(float f32) {
  if (!(std::abs(f32) >= 1.0f / 512.0f)) {
    return 0;
  }
  if (f32 >= 32768.0f - 1.0f / 256.0f) {
    return (1 << 23) - 1;
  }
  if (f32 <= -32768.0f) {
    return -32768 * 256;
  }
  uint32_t f32_bits = std::bit_cast<uint32_t>(f32);
  union {
    int32_t s;
    uint32_t u;
  } result;
  result.u = (f32_bits & ((1 << 23) - 1)) | (1 << 23);
  if ((f32_bits >> 31) != 0) {
    result.s = -result.s;
  }
  int32_t exponent = static_cast<int32_t>((f32_bits >> 23) & 255) - 127;
  uint32_t extra_bits = static_cast<uint32_t>(15 - exponent);
  if (extra_bits) {
    result.u += (1 << (extra_bits - 1)) - 1 + ((result.u >> extra_bits) & 1);
    result.s >>= extra_bits;
  }
  return result.s;
}

}  // namespace gpu
}  // namespace rex
