/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cmath>

#include <rex/bit.h>
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::xenos {

f32 PWLGammaToLinear(f32 gamma) {
  gamma = rex::saturate(gamma);
  f32 scale, offset;
  // value >= constant is false for NaN, so NaN takes the lowest piece.
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (gamma >= 64.0f / 255.0f) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
    }
  }
  f32 linear = gamma * ((255.0f * 1024.0f) * scale) + offset;
  linear += std::trunc(linear * scale);
  linear *= 1.0f / 1023.0f;
  return linear;
}

f32 LinearToPWLGamma(f32 linear) {
  linear = rex::saturate(linear);
  f32 scale, offset;
  // value >= constant is false for NaN, so NaN takes the lowest piece.
  if (linear >= 128.0f / 1023.0f) {
    if (linear >= 512.0f / 1023.0f) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (linear >= 64.0f / 1023.0f) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  // Truncation maps 1.0 to exactly 1.0.
  return std::trunc(linear * scale) * (1.0f / 255.0f) + offset;
}

f32 Float7e3To32(u32 f10) {
  f10 &= 0x3FF;
  if (!f10) {
    return 0.0f;
  }
  u32 mantissa = f10 & 0x7F;
  u32 exponent = f10 >> 7;
  if (!exponent) {
    u32 mantissa_lzcnt = rex::lzcnt(mantissa) - (32 - 8);
    exponent = static_cast<u32>(1 - static_cast<i32>(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7F;
  }
  return rex::Reinterpret<f32>(((exponent + 124) << 23) | (mantissa << 16));
}

u32 Float32To20e4(f32 value, bool round_to_nearest_even) {
  if (!(value > 0.0f)) {
    return 0;
  }
  auto f32u32 = rex::Reinterpret<u32>(value);
  if (f32u32 >= 0x3FFFFFF8) {
    return 0xFFFFFF;
  }
  if (f32u32 < 0x38800000) {
    u32 shift = std::min(static_cast<u32>(113 - (f32u32 >> 23)), static_cast<u32>(24));
    f32u32 = (0x800000 | (f32u32 & 0x7FFFFF)) >> shift;
  } else {
    f32u32 += 0xC8000000u;
  }
  if (round_to_nearest_even) {
    f32u32 += 3 + ((f32u32 >> 3) & 1);
  }
  return (f32u32 >> 3) & 0xFFFFFF;
}

f32 Float20e4To32(u32 f24) {
  f24 &= 0xFFFFFF;
  if (!f24) {
    return 0.0f;
  }
  u32 mantissa = f24 & 0xFFFFF;
  u32 exponent = f24 >> 20;
  if (!exponent) {
    u32 mantissa_lzcnt = rex::lzcnt(mantissa) - (32 - 21);
    exponent = static_cast<u32>(1 - static_cast<i32>(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFF;
  }
  return rex::Reinterpret<f32>(((exponent + 112) << 23) | (mantissa << 3));
}

const char* GetColorRenderTargetFormatName(ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
      return "k_8_8_8_8";
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return "k_8_8_8_8_GAMMA";
    case ColorRenderTargetFormat::k_2_10_10_10:
      return "k_2_10_10_10";
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      return "k_2_10_10_10_FLOAT";
    case ColorRenderTargetFormat::k_16_16:
      return "k_16_16";
    case ColorRenderTargetFormat::k_16_16_16_16:
      return "k_16_16_16_16";
    case ColorRenderTargetFormat::k_16_16_FLOAT:
      return "k_16_16_FLOAT";
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return "k_16_16_16_16_FLOAT";
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return "k_2_10_10_10_AS_10_10_10_10";
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return "k_2_10_10_10_FLOAT_AS_16_16_16_16";
    case ColorRenderTargetFormat::k_32_FLOAT:
      return "k_32_FLOAT";
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return "k_32_32_FLOAT";
    default:
      return "kUnknown";
  }
}

const char* GetDepthRenderTargetFormatName(DepthRenderTargetFormat format) {
  switch (format) {
    case DepthRenderTargetFormat::kD24S8:
      return "kD24S8";
    case DepthRenderTargetFormat::kD24FS8:
      return "kD24FS8";
    default:
      return "kUnknown";
  }
}

}  // namespace rex::xenos
