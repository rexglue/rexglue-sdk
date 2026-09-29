/**
 * @file        input.h
 * @brief       Input module facade and configuration slice
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/input/input.h>

namespace rex::input {

/** Controller backend. */
enum class Backend : uint8_t {
  SDL,    /**< SDL3 gamepads, every platform. */
  XInput, /**< XInput, Windows only. */
};

/** Input module slice. */
struct InputConfig {
  /** Which controller backend reads the pads. XInput is rejected in Init on other platforms. */
  Backend backend = Backend::SDL;
  /**
   * SDL gamepad mapping file. A relative path resolves against the executable folder. Empty
   * disables.
   */
  std::filesystem::path hid_mappings_file = "gamecontrollerdb.txt";
};

}  // namespace rex::input
