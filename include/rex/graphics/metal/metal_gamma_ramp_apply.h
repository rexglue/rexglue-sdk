/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_GAMMA_RAMP_APPLY_H_
#define XENIA_GPU_METAL_METAL_GAMMA_RAMP_APPLY_H_

// CPU oracle for the standard 256-entry guest gamma ramp, and the packed upload
// layout the Metal shader consumes.
//
// This is the SINGLE definition of the lookup contract. The Metal shader in
// metal_command_processor.mm implements the same arithmetic against the same
// packed buffer, and conformance compares the on-device result to this oracle
// pixel for pixel. Neither may be "fixed" without the other.
//
// THE CONTRACT, from apply_gamma_table.xesli via the 2026-07-29 audit:
//
//   idx_c = uint(src_c * 255 + 0.5)                 // D3D10+ UNORM rules
//   out_c = table[idx_c].color_10_c / 1023          // per channel, independent
//   out_a = 1.0
//
// with NO interpolation and NO saturate -- both exist only in the PWL path,
// which is out of scope. Each channel indexes with its OWN value; there is no
// shared index.
//
// CHANNEL ORDER -- THE EASIEST THING TO GET WRONG
// ------------------------------------------------
// DC_LUT_30_COLOR packs blue in bits 0:9, green in 10:19, red in 20:29. D3D12
// binds the table as a TYPED R10G10B10A2_UNORM buffer, so its `.r` lands on
// guest blue and its `.b` on guest red; the shader's
// `out.r = ramp[in.r].b` therefore CANCELS the packing rather than swapping
// channels. Net of it all, red comes from red.
//
// This header reads the fields by name and so must NOT reverse anything. A
// greyscale ramp cannot detect a mistake here, which is why conformance
// requires asymmetric per-channel curves.
//
// 8-BIT QUANTIZATION
// ------------------
// The reference writes a 10-bit rgb10_a2 destination. Metal's guest output
// texture is RGBA8Unorm, so the result is quantized to 8 bits. That is a
// pre-existing property of Metal's guest output, not something introduced by
// applying the ramp, and the oracle must model it or it would disagree with
// the device on essentially every midtone.
//
// Note that all three identity ramps round-trip through this quantization
// exactly, for all 256 inputs -- so "identity implies byte-identical output" is
// an exact claim here, not a tolerance.

#include <cstdint>

#include <rex/xenos/register.h>

namespace rex {
namespace graphics {
namespace metal {

// Number of bytes uploaded per ramp. One dword per entry, exactly the guest
// register layout, so the shader unpacks the same bits the guest wrote.
inline constexpr uint32_t kGammaRampTableEntries = 256;
inline constexpr uint32_t kGammaRampTableBytes =
    kGammaRampTableEntries * sizeof(uint32_t);

// The 8-bit index a channel value selects. `src` is the normalized value read
// from an 8-bit UNORM source, so this is exact.
inline uint32_t GammaRampIndexFromUnorm8(float src) {
  const float scaled = src * 255.0f + 0.5f;
  if (scaled <= 0.0f) {
    return 0;
  }
  const uint32_t index = uint32_t(scaled);
  return index > 255u ? 255u : index;
}

// The 10-bit ramp output for one channel, read by field name (see above: do not
// reverse). `channel` is 0=red, 1=green, 2=blue.
inline uint32_t GammaRampLookup10(const reg::DC_LUT_30_COLOR* table,
                                  uint32_t index, uint32_t channel) {
  const reg::DC_LUT_30_COLOR entry = table[index];
  switch (channel) {
    case 0:
      return entry.color_10_red;
    case 1:
      return entry.color_10_green;
    default:
      return entry.color_10_blue;
  }
}

// Round-to-nearest 10-bit -> 8-bit UNORM, matching the hardware conversion
// performed when the shader's float output is stored to RGBA8Unorm.
//
// No exact-half case exists: out10 * 255 / 1023 is a half-integer only if
// out10 * 510 is an odd multiple of 1023, which is impossible because the left
// side is even and 1023 is odd. Nearest is therefore unambiguous and no
// tie-breaking rule needs to be agreed with the device.
inline uint8_t GammaRampQuantize8(uint32_t out10) {
  return uint8_t((out10 * 255u + 511u) / 1023u);
}

// The complete oracle for one pixel: 8-bit RGB in, 8-bit RGB out. Alpha is
// forced to 255 (the shader writes 1.0), matching the reference.
struct GammaRampPixel {
  uint8_t r, g, b, a;
};

inline GammaRampPixel ApplyGammaRampOracle(const reg::DC_LUT_30_COLOR* table,
                                           uint8_t r, uint8_t g, uint8_t b) {
  GammaRampPixel out;
  out.r = GammaRampQuantize8(GammaRampLookup10(
      table, GammaRampIndexFromUnorm8(r / 255.0f), 0));
  out.g = GammaRampQuantize8(GammaRampLookup10(
      table, GammaRampIndexFromUnorm8(g / 255.0f), 1));
  out.b = GammaRampQuantize8(GammaRampLookup10(
      table, GammaRampIndexFromUnorm8(b / 255.0f), 2));
  out.a = 255;
  return out;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_GAMMA_RAMP_APPLY_H_
