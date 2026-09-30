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

#include <filesystem>

#include <rex/input/input.h>
#include <rex/types.h>

namespace rex::input {

/** Controller backend. */
enum class Backend : u8 {
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
  /** True maps every controller to user 0. False gives each controller its own user. */
  bool shared_pads = false;
};

}  // namespace rex::input
