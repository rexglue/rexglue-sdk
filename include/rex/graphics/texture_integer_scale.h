/**
 * Packed integer-scale bits for the Metal shader's system constants.
 *
 * Upstream publishes xenos::FormatInfo (xenos/texture.h) but not the per-component widths of the fixed-point formats, so
 * the rows below are the fork's own table entries copied verbatim. Everything else - the point-sampled test, the
 * normalized bit, the sign/width packing - is the fork's algorithm unchanged.
 *
 * Delete this file if upstream ever publishes component widths for TextureFormat.
 */
#pragma once

#include <cstdint>

#include <rex/xenos/fetch.h>
#include <rex/xenos/texture.h>

#include "rex/graphics/texture_sign.h"

namespace rex::graphics::texture_integer_scale {

// Component bit widths of the fixed-point formats, in the order the fork's format table lists them.
constexpr uint32_t ComponentBits(xenos::TextureFormat format, uint32_t component) {
  switch (format) {
    case xenos::TextureFormat::k_8:
      return component == 0 ? 8 : 0;
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      return component == 0 ? 8 : 0;
    case xenos::TextureFormat::k_8_8:
      return component < 2 ? 8 : 0;
    case xenos::TextureFormat::k_16:
      return component == 0 ? 16 : 0;
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_EDRAM:
      return component < 2 ? 16 : 0;
    case xenos::TextureFormat::k_16_16_16_16:
      return 16;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
      return 8;
    case xenos::TextureFormat::k_2_10_10_10:
      return component < 3 ? 10 : 2;
    case xenos::TextureFormat::k_1_5_5_5: {
      constexpr uint32_t kBits[4] = {5, 5, 5, 1};
      return kBits[component & 3];
    }
    case xenos::TextureFormat::k_5_6_5: {
      constexpr uint32_t kBits[4] = {5, 6, 5, 0};
      return kBits[component & 3];
    }
    case xenos::TextureFormat::k_6_5_5: {
      constexpr uint32_t kBits[4] = {5, 5, 6, 0};
      return kBits[component & 3];
    }
    case xenos::TextureFormat::k_10_11_11: {
      constexpr uint32_t kBits[4] = {11, 11, 10, 0};
      return kBits[component & 3];
    }
    case xenos::TextureFormat::k_11_11_10: {
      constexpr uint32_t kBits[4] = {10, 11, 11, 0};
      return kBits[component & 3];
    }
    case xenos::TextureFormat::k_4_4_4_4:
      return 4;
    default:
      return 0u;  // not a fixed-point format
  }
}

inline uint32_t IntegerScaleBits(const xenos::TextureFetch& fetch, uint8_t swizzled_signs) {
  const bool point_sampled =
      fetch.mag_filter == xenos::TextureFilter::kPoint &&
      fetch.min_filter == xenos::TextureFilter::kPoint &&
      (fetch.mip_filter == xenos::TextureFilter::kPoint ||
       fetch.mip_filter == xenos::TextureFilter::kBaseMap) &&
      fetch.aniso_filter == xenos::AnisoFilter::kDisabled;
  uint32_t scale_bits = point_sampled ? texture_util::kIntegerScalePointSampledBit : 0;

  uint32_t last_stored_component = 0;
  for (uint32_t i = 1; i < 4; ++i) {
    if (ComponentBits(fetch.format, i)) {
      last_stored_component = i;
    }
  }
  if (!last_stored_component && !ComponentBits(fetch.format, 0)) {
    return scale_bits;  // not fixed-point, so nothing else applies
  }

  if (!fetch.num_format) {
    scale_bits |= texture_util::kIntegerScaleNormalizedBit;
  }

  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t source_component = (fetch.swizzle >> (i * 3)) & 0b111;
    if (source_component >= xenos::REXGPU_SWIZZLE_0) {
      continue;
    }
    source_component = source_component < last_stored_component ? source_component : last_stored_component;

    xenos::TextureSign sign = xenos::TextureSign((swizzled_signs >> (i * 2)) & 0b11);

    uint32_t width = ComponentBits(fetch.format, source_component);
    if (!width || width > 16) {
      continue;
    }

    bool carries_width = true;
    if (sign == xenos::TextureSign::kGamma) {
      if (fetch.num_format) {
        continue;
      }
      carries_width = false;
    } else if (!fetch.num_format && sign == xenos::TextureSign::kUnsigned) {
      carries_width = point_sampled && width >= 4 && width <= 7;
    }

    uint32_t component_scale = uint32_t(sign) << texture_util::kIntegerScaleSignShift;
    if (carries_width) {
      component_scale |= width - 1;
    }
    scale_bits |= component_scale << (i * texture_util::kIntegerScaleBitsPerComponent);
  }

  return scale_bits;
}

}  // namespace rex::graphics::texture_integer_scale
