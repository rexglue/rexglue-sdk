/**
 * @file        title.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {

struct VideoMode {
  u32 width = 1280;
  u32 height = 720;
  f32 refresh_rate = 60.0f;
};

/** Title ID and video mode, passed to systems and drivers at attach. */
struct TitleInfo {
  u32 title_id = 0; /**< 0 when the image has none. */
  VideoMode video_mode;
};

struct Achievement {
  u32 id = 0;
  std::string label;
  std::string description;
  std::string unachieved_description;
  std::string icon_path;
  u32 image_id = 0; /**< XDBF image ID of the icon. */
  u32 gamerscore = 0;
  u32 flags = 0;
  bool unlocked = false;
  u64 unlock_time = 0; /**< FILETIME, 0 when locked. */

  /** Returns the text shown for the achievement, otherwise the unachieved description. */
  const std::string& GetDescription() const {
    return unlocked ? description : unachieved_description;
  }
};

class IAchievementRegistry {
 public:
  virtual ~IAchievementRegistry() = default;

  /**
   * Lists the achievements with their unlock state.
   *
   * @return The achievements, empty when the title has none.
   */
  virtual std::vector<Achievement> GetAchievements() = 0;

  /** Returns an icon as PNG bytes, or kNotFound. */
  [[nodiscard]] virtual Result<std::vector<std::byte>> GetAchievementIcon(u32 image_id) = 0;

  /**
   * Calls a function when an achievement unlocks, on the unlocking thread.
   *
   * @return The guard, destroyed to stop the calls.
   */
  [[nodiscard]] virtual ScopeGuard OnAchievementUnlocked(
      MoveOnlyFunction<void(const Achievement&)> fn) = 0;
};

}  // namespace rex
