/**
 * @file        runtime/achievement_info.h
 * @brief       Shared achievement data type used by both the CLI and runtime.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once
#include <cstdint>
#include <string>

#include <rex/types.h>

namespace rex::runtime {

/** 1 achievement a title can unlock. */
struct Achievement {
  u32 id = 0;                         /**< Achievement id the title unlocks by. */
  std::string label;                  /**< Short name shown to the player. */
  std::string description;            /**< Text shown once the achievement is unlocked. */
  std::string unachieved_description; /**< Text shown while the achievement is locked. */
  std::string icon_path;              /**< Path of the icon image. */
  u32 image_id = 0;                   /**< Image id the title refers to the icon by. */
  u32 gamerscore = 0;                 /**< Gamerscore the achievement is worth. */
  u32 flags = 0;                      /**< Achievement flag bits. */
};

}  // namespace rex::runtime
