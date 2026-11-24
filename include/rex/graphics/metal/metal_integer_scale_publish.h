/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_INTEGER_SCALE_PUBLISH_H_
#define XENIA_GPU_METAL_METAL_INTEGER_SCALE_PUBLISH_H_

// Publication of per-fetch-constant integer texture scales into the shader
// system constants
// (openspec/changes/implement-unsigned-integer-texture-scaling).
//
// This is a separate header, rather than a loop inlined in the command
// processor, because its invariants are exactly the ones a rendered frame
// cannot show you:
//
//   - the value lands at the BINDING'S OWN fetch-constant index. The engaged
//     Just Cause 2 consumers sit at tf6 and tf10 (and tf4 in one trace), never
//     at slot 0, so an off-by-one or a "first binding wins" bug would still
//     look plausible in a frame while feeding the wrong shader lane.
//   - every slot NOT bound by the draw stays zero, which is what makes the
//     shader take its uniform early-out. A stray non-zero in a gap silently
//     rescales an unrelated fetch.
//   - a stage with no integer fetches contributes nothing, so vertex and pixel
//     stages sharing the array cannot corrupt each other.
//
// The command processor and the conformance suite call the same functions here,
// so a test cannot pass against a re-implementation of the rule.

#include "rex/graphics/texture_integer_scale.h"
#include <cstddef>
#include <cstdint>

#include "rex/graphics/texture_util.h"
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// One entry per guest texture fetch constant.
inline constexpr size_t kMetalIntegerScaleSlotCount = 32;

// Clears the whole array. Callers that memset the entire system-constant block
// already satisfy this; it exists so the invariant is stated once and can be
// asserted directly.
inline void ResetIntegerScaleBits(uint32_t* scale_bits, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    scale_bits[i] = 0;
  }
}

// kIntegerScaleSnapMipsBit for a point sampled constant (bit 26 set in
// scale) whose texture has more than one usable mip level, else 0.
inline uint32_t SnapMipsBit(const xenos::TextureFetch& fetch,
                            uint32_t scale) {
  if (!(scale & texture_util::kIntegerScalePointSampledBit)) {
    return 0;
  }
  uint32_t mip_min_level, mip_max_level;
  ::rex::xenos::GetSubresourcesFromFetchConstant(
      fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &mip_min_level,
      &mip_max_level);
  return mip_max_level > mip_min_level ? texture_util::kIntegerScaleSnapMipsBit
                                       : 0;
}

// Publishes one bound texture's packed fixed conversion
// (texture_util::IntegerScaleBits) at its own fetch-constant index.
//
// Returns the value written, 0 when the binding needs no conversion. Out-of-
// range fetch constants are ignored rather than clamped: clamping would write a
// real scale onto an unrelated slot, which is worse than dropping it.
inline uint32_t PublishIntegerScaleBits(
    uint32_t fetch_constant, const xenos::TextureFetch& fetch,
    uint8_t swizzled_signs, uint32_t* scale_bits, size_t count,
    bool snap_mips = false) {
  if (fetch_constant >= count) {
    return 0;
  }
  uint32_t scale = texture_integer_scale::IntegerScaleBits(fetch, swizzled_signs);
  if (snap_mips) {
    scale |= SnapMipsBit(fetch, scale);
  }
  scale_bits[fetch_constant] = scale;
  return scale;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_INTEGER_SCALE_PUBLISH_H_
