/**
 * @file        rex/runtime/achievement_info.h
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

namespace rex::runtime {

struct AchievementInfo {
  uint32_t id = 0;
  std::string label;
  std::string description;
  std::string unachieved_description;
  std::string icon_path;
  uint32_t image_id = 0;
  uint32_t gamerscore = 0;
  uint32_t flags = 0;
};

}  // namespace rex::runtime
