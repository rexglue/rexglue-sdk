/**
 * The Metal backend's fetch-pattern analysis result.
 *
 * Upstream has no equivalent: this backend added the analysis, and both the shader (which produces it) and the command
 * processor (which caches 32 slots) need the type, so it lives in its own header.
 */
#pragma once

#include <cstdint>

namespace rex::graphics::metal {

struct MetalFetchStaticInfo {
  uint8_t swizzled_signs;
  int8_t exp_adjust;
  uint32_t integer_scale_bits;
};

}  // namespace rex::graphics::metal
