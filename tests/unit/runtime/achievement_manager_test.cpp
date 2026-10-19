/**
 * @file        tests/unit/runtime/achievement_manager_test.cpp
 * @brief       Unit tests for achievement metadata, persistence, and events.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "rex_test.h"

#include <rex/api.h>
#include <rex/runtime.h>
#include <rex/embedded_metadata.h>
#include "runtime/achievement_manager.h"
#include "temp_folder.h"
#include "file_text.h"

namespace {

using rex::test::WriteText;

rex::runtime::AchievementInfo BuildAchievement(u32 id, std::string label) {
  rex::runtime::AchievementInfo info;
  info.id = id;
  info.label = std::move(label);
  info.description = "Unlocked";
  info.unachieved_description = "Locked";
  info.image_id = id + 100;
  info.gamerscore = 10;
  return info;
}

}  // namespace

REX_TEST_CASE("achievement metadata layers embedded, TOML, and recomp entries",
              "[runtime][achievements]") {
  rex::test::TempFolder temp("rex_achievement_metadata_");
  auto metadata_path = temp.path / "achievements.toml";
  WriteText(metadata_path, R"(
[[achievements]]
id = 1
label = "Metadata override"
description = "Metadata unlocked"
unachieved_description = "Metadata locked"
icon_path = "custom/one.png"
image_id = 101
gamerscore = 20
flags = 3

[[achievements]]
id = 2
label = "Metadata addition"
description = "Second unlocked"
unachieved_description = "Second locked"
image_id = 202
gamerscore = 30
flags = 4
)");

  rex::runtime::AchievementManager manager;
  manager.ReplaceAchievements({BuildAchievement(1, "Embedded")});
  REQUIRE(manager.LoadMetadataFile(metadata_path).has_value());

  const auto achievements = manager.ListAchievements();
  const auto first = std::ranges::find(achievements, 1u, &rex::runtime::AchievementInfo::id);
  REQUIRE(first != achievements.end());
  CHECK(first->label == "Metadata override");
  CHECK(first->icon_path == "custom/one.png");
  CHECK(achievements.size() == 2);

  REQUIRE(manager.RegisterAchievement(BuildAchievement(2, "Recomp override")).has_value());
  const auto updated = manager.ListAchievements();
  const auto second = std::ranges::find(updated, 2u, &rex::runtime::AchievementInfo::id);
  REQUIRE(second != updated.end());
  CHECK(second->label == "Recomp override");
}

REX_TEST_CASE("an achievement batch with an invalid id registers nothing",
              "[runtime][achievements]") {
  rex::runtime::AchievementManager manager;
  const std::array batch{BuildAchievement(1, "One"), BuildAchievement(0, "Reserved"),
                         BuildAchievement(2, "Two")};
  const auto registered = manager.RegisterAchievements(batch);
  REQUIRE_FALSE(registered);
  CHECK(registered.error().category == rex::ErrorCategory::kValidation);
  CHECK(manager.ListAchievements().empty());
}

REX_TEST_CASE("a valid achievement batch registers every entry", "[runtime][achievements]") {
  rex::runtime::AchievementManager manager;
  const std::array batch{BuildAchievement(1, "One"), BuildAchievement(2, "Two")};
  REQUIRE(manager.RegisterAchievements(batch));
  CHECK(manager.ListAchievements().size() == 2);
}

REX_TEST_CASE("achievement notifications follow the unlock request", "[runtime][achievements]") {
  rex::runtime::AchievementManager manager;
  REQUIRE(manager.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  REQUIRE(manager.RegisterAchievement(BuildAchievement(2, "Two")).has_value());

  int notification_count = 0;
  auto notification_listener = manager.RegisterNotificationCallback(
      [&](const rex::runtime::AchievementEvent&) { ++notification_count; });

  CHECK(manager.UnlockAchievement(1) == rex::runtime::AchievementUnlockResult::kUnlocked);
  CHECK(notification_count == 0);

  CHECK(manager.UnlockAchievement(1, rex::runtime::AchievementNotification::kShow) ==
        rex::runtime::AchievementUnlockResult::kAlreadyUnlocked);
  CHECK(notification_count == 0);

  CHECK(manager.UnlockAchievement(2, rex::runtime::AchievementNotification::kShow) ==
        rex::runtime::AchievementUnlockResult::kUnlocked);
  CHECK(notification_count == 1);

  manager.UnregisterCallback(notification_listener);
  CHECK(manager.UnlockAchievement(3) == rex::runtime::AchievementUnlockResult::kUnknownAchievement);
  CHECK(notification_count == 1);
}

REX_TEST_CASE("achievement unlock state persists across manager instances",
              "[runtime][achievements]") {
  rex::test::TempFolder temp("rex_achievement_save_");
  auto save_path = temp.path / "saves" / "12345678.toml";

  rex::runtime::AchievementManager first;
  REQUIRE(first.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  first.SetUnlockSavePath(save_path);
  REQUIRE(first.UnlockAchievement(1) == rex::runtime::AchievementUnlockResult::kUnlocked);
  REQUIRE(std::filesystem::exists(save_path));
  auto unlock_time = first.GetUnlockTime(1);
  REQUIRE(unlock_time.has_value());

  rex::runtime::AchievementManager second;
  REQUIRE(second.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  second.SetUnlockSavePath(save_path);
  second.LoadUnlockState();
  CHECK(second.IsUnlocked(1));
  CHECK(second.GetUnlockTime(1) == unlock_time);
}

REX_TEST_CASE("achievement unlock saves serialize concurrent writers", "[runtime][achievements]") {
  rex::test::TempFolder temp("rex_achievement_concurrent_save_");
  auto save_path = temp.path / "saves" / "12345678.toml";

  rex::runtime::AchievementManager manager;
  manager.SetUnlockSavePath(save_path);
  for (u32 id = 1; id <= 8; ++id) {
    REQUIRE(manager.RegisterAchievement(BuildAchievement(id, "Achievement " + std::to_string(id)))
                .has_value());
  }

  std::vector<std::thread> threads;
  for (u32 id = 1; id <= 8; ++id) {
    threads.emplace_back([&manager, id]() { static_cast<void>(manager.UnlockAchievement(id)); });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  rex::runtime::AchievementManager reloaded;
  reloaded.SetUnlockSavePath(save_path);
  for (u32 id = 1; id <= 8; ++id) {
    REQUIRE(reloaded.RegisterAchievement(BuildAchievement(id, "Achievement " + std::to_string(id)))
                .has_value());
  }
  reloaded.LoadUnlockState();
  for (u32 id = 1; id <= 8; ++id) {
    CHECK(reloaded.IsUnlocked(id));
  }
}

REX_TEST_CASE("runtime metadata root overrides legacy metadata discovery",
              "[runtime][achievements]") {
  rex::test::TempFolder temp("rex_metadata_root_");
  auto game_root = temp.path / "game";
  auto local_metadata = game_root / "metadata" / "achievements.toml";
  auto parent_metadata = temp.path / "metadata" / "achievements.toml";
  auto explicit_metadata = temp.path / "custom" / "achievements.toml";
  WriteText(local_metadata, "local");
  WriteText(parent_metadata, "parent");
  WriteText(explicit_metadata, "explicit");

  {
    rex::Config config;
    config.paths.game_data_root = game_root;
    rex::Runtime runtime(config);
    CHECK(runtime.FindMetadataPath("achievements.toml") == local_metadata);

    std::filesystem::remove(local_metadata);
    CHECK(runtime.FindMetadataPath("achievements.toml") == parent_metadata);
  }

  {
    rex::Config config;
    config.paths.game_data_root = game_root;
    config.paths.metadata_root = temp.path / "custom";
    rex::Runtime runtime(config);
    CHECK(runtime.FindMetadataPath("achievements.toml") == explicit_metadata);
    CHECK_FALSE(runtime.FindMetadataPath("missing.toml"));
  }
}

REX_TEST_CASE("embedded metadata assets resolve by metadata relative path",
              "[runtime][achievements]") {
  static constexpr u8 kIconBytes[] = {0x89, 0x50, 0x4E, 0x47};
  REQUIRE(rex::RegisterEmbeddedMetadataAsset("icons/custom/unit-test.png", kIconBytes,
                                             sizeof(kIconBytes)));

  auto asset =
      rex::FindEmbeddedMetadataAsset(std::filesystem::path("icons") / "custom" / "unit-test.png");
  if (!asset) {
    FAIL("the embedded asset is not registered");
    return;
  }
  REQUIRE(asset->bytes.size() == sizeof(kIconBytes));
  CHECK(asset->bytes[0] == 0x89);
  CHECK(asset->bytes[1] == 0x50);
  CHECK(asset->bytes[2] == 0x4E);
  CHECK(asset->bytes[3] == 0x47);
}

REX_TEST_CASE("the unlock save is found under names that differ in case",
              "[runtime][achievements][case]") {
  TempDirectory temp("rex_achievement_save_case");
  std::filesystem::create_directories(temp.GetPath() / "Achievements");
  std::ofstream(temp.GetPath() / "Achievements" / "abcdef01.toml") << "";
  const auto found = rex::runtime::ResolveUnlockSavePath(temp.GetPath(), 0xABCDEF01);
  CHECK(std::filesystem::exists(found));
}
