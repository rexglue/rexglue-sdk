/**
 * @file        ui/overlay/achievement_groups.h
 *
 * @brief       Original/custom split, ordering and totals for the achievements overlay.
 */

#pragma once

#include <algorithm>
#include <vector>

#include <rex/system/achievement_store.h>

namespace rex::ui::achievement_groups {

struct Groups {
  std::vector<rex::system::AchievementInfo> original;
  std::vector<rex::system::AchievementInfo> custom;
};

struct Totals {
  int unlocked = 0;
  int total = 0;
  int earned_gamerscore = 0;
  int total_gamerscore = 0;
};

// Original keeps catalog order. Custom sorts by ascending gamerscore, then label, then ID.
// Equal entries keep their order.
inline Groups Split(const std::vector<rex::system::AchievementInfo>& achievements) {
  Groups groups;
  for (const auto& achievement : achievements) {
    (achievement.custom ? groups.custom : groups.original).push_back(achievement);
  }
  std::stable_sort(groups.custom.begin(), groups.custom.end(), [](const auto& a, const auto& b) {
    if (a.gamerscore != b.gamerscore) {
      return a.gamerscore < b.gamerscore;
    }
    if (a.label != b.label) {
      return a.label < b.label;
    }
    return a.id < b.id;
  });
  return groups;
}

template <typename IsUnlocked>
Totals Summarize(const std::vector<rex::system::AchievementInfo>& achievements,
                 IsUnlocked&& is_unlocked) {
  Totals totals;
  totals.total = static_cast<int>(achievements.size());
  for (const auto& achievement : achievements) {
    totals.total_gamerscore += static_cast<int>(achievement.gamerscore);
    if (is_unlocked(achievement.id)) {
      ++totals.unlocked;
      totals.earned_gamerscore += static_cast<int>(achievement.gamerscore);
    }
  }
  return totals;
}

}  // namespace rex::ui::achievement_groups
