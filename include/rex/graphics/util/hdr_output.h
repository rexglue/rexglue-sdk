/**
 * @file        rex/graphics/util/hdr_output.h
 * @brief       Helpers for the HDR output path (hdr_output): color conversions shared by the
 *              host backends.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cmath>
#include <cstdint>

namespace rex::graphics::util {

// Converts a value in [0, 1] to Xenos 7e3 bits (bits 0:9), rounding to the nearest representable
// value. For [0, 1] this is the inverse of xenos::Float7e3To32. Values outside [0, 1] are clamped.
inline uint32_t UnormToFloat7e3(float value) {
  if (!(value > 0.0f)) {
    return 0;
  }
  if (value >= 1.0f) {
    return 3u << 7;  // 1.0: exponent 3, mantissa 0.
  }
  if (value < 0.25f) {
    // Denormal: value = mantissa / 512. 0.25 - 1/1024 and up round to 128, which is the bit
    // pattern of the smallest normal value, 0.25.
    return uint32_t(std::lround(value * 512.0f));
  }
  int exponent;
  float fraction = std::frexp(value, &exponent);  // value = fraction * 2^exponent, [0.5, 1).
  uint32_t biased = uint32_t(exponent + 2);
  uint32_t mantissa = uint32_t(std::lround((fraction * 2.0f - 1.0f) * 128.0f));
  if (mantissa == 128) {
    mantissa = 0;
    ++biased;
  }
  return (biased << 7) | mantissa;
}

// Re-packs a color clear value packed for a 32bpp k_8_8_8_8 (is_8888) or k_2_10_10_10 render
// target as k_2_10_10_10_FLOAT, for clearing a render target that the HDR output path stores as
// 7e3 on the host.
inline uint32_t RepackColorClearAsFloat7e3(uint32_t packed, bool is_8888) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    float value = is_8888 ? float((packed >> (8 * i)) & 0xFF) * (1.0f / 255.0f)
                          : float((packed >> (10 * i)) & 0x3FF) * (1.0f / 1023.0f);
    result |= UnormToFloat7e3(value) << (10 * i);
  }
  uint32_t alpha =
      is_8888 ? uint32_t(std::lround(float(packed >> 24) * (3.0f / 255.0f))) : packed >> 30;
  return result | (alpha << 30);
}

}  // namespace rex::graphics::util
