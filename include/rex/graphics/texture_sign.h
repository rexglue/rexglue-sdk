/**
 * Packed texture-sign helpers.
 *
 * Upstream publishes the tiled-address maths under rex/xenos/texture.h but not these two predicates, which the ported
 * Metal backend uses to decide whether a fetch is signed. They are copied verbatim from the upstream fork so the bit
 * arithmetic is the same one the other backends use.
 */
#pragma once

#include <cstdint>

#include <rex/xenos/fetch.h>

namespace rex::graphics::texture_util {

// True when at least one component of the packed swizzle is not signed.
constexpr bool IsAnySignNotSigned(uint8_t packed_signs) {
  return packed_signs != uint32_t(xenos::TextureSign::kSigned) * 0b01010101;
}

// True when at least one component is signed: make signed 00 and check the rest are 01, 10 or 11.
constexpr bool IsAnySignSigned(uint8_t packed_signs) {
  uint32_t xor_signed = packed_signs ^ (uint32_t(xenos::TextureSign::kSigned) * 0b01010101);
  return ((xor_signed | (xor_signed >> 1)) & 0b01010101) != 0b01010101;
}


// Layout of the packed fixed texture conversion, which upstream does not publish: four 6-bit fields in bits 0:23, one per
// output component; bit 24 marks a normalized number format and bit 26 a point-sampled fetch.
constexpr uint32_t kIntegerScaleBitsPerComponent = 6;
constexpr uint32_t kIntegerScaleWidthMask = 0xF;
constexpr uint32_t kIntegerScaleSignShift = 4;
constexpr uint32_t kIntegerScaleNormalizedBit = UINT32_C(1) << 24;
constexpr uint32_t kIntegerScalePointSampledBit = UINT32_C(1) << 26;
constexpr uint32_t kIntegerScaleSnapMipsBit = UINT32_C(1) << 27;

}  // namespace rex::graphics::texture_util