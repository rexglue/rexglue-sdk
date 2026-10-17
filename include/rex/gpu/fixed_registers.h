/**
 * @file        gpu/fixed_registers.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <optional>

#include <rex/types.h>
#include <rex/ui/display_mode.h>
#include <rex/xenos/registers.h>

namespace rex::gpu {

/** Largest width or height the viewport size register reports. */
inline constexpr u32 kViewportLimit = 0x0FFF;

/**
 * Gives the value a title reads from a register that reports the display mode or a fixed value.
 *
 * @param index The register index.
 * @param mode The display mode.
 * @return The value, or empty when the register is not one of these.
 */
inline std::optional<u32> ReadFixedRegister(u32 index, const ui::VideoMode& mode) {
  switch (index) {
    case static_cast<u32>(xenos::Register::RB_EDRAM_TIMING):
      return 0x08100748;
    case static_cast<u32>(xenos::Register::RB_BC_CONTROL):
      return 0x0000200E;
    case static_cast<u32>(xenos::Register::D1MODE_V_COUNTER):
      return std::min(mode.height, kViewportLimit);
    case static_cast<u32>(xenos::Register::D1MODE_VBLANK_VLINE_STATUS):
      return 1;
    case static_cast<u32>(xenos::Register::AVIVO_D1MODE_VIEWPORT_SIZE):
      return (std::min(mode.width, kViewportLimit) << 16) | std::min(mode.height, kViewportLimit);
    default:
      return std::nullopt;
  }
}

}  // namespace rex::gpu
