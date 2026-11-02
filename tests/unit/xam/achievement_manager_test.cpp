/**
 * @file        tests/unit/xam/achievement_manager_test.cpp
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

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/ppc/image.h>
#include "xam/achievement_manager.h"
#include "kernel/title_fixture.h"
#include "temp_folder.h"
#include "file_text.h"
#include "kernel/title_state.h"

namespace {

using rex::test::WriteText;

rex::Achievement BuildAchievement(u32 id, std::string label) {
  rex::Achievement info;
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
              "[xam][achievements]") {
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

  rex::xam::AchievementManager manager;
  manager.ReplaceAchievements({BuildAchievement(1, "Embedded")});
  REQUIRE(manager.LoadMetadataFile(metadata_path).has_value());

  const auto achievements = manager.ListAchievements();
  const auto first = std::ranges::find(achievements, 1u, &rex::Achievement::id);
  REQUIRE(first != achievements.end());
  CHECK(first->label == "Metadata override");
  CHECK(first->icon_path == "custom/one.png");
  CHECK(achievements.size() == 2);

  REQUIRE(manager.RegisterAchievement(BuildAchievement(2, "Recomp override")).has_value());
  const auto updated = manager.ListAchievements();
  const auto second = std::ranges::find(updated, 2u, &rex::Achievement::id);
  REQUIRE(second != updated.end());
  CHECK(second->label == "Recomp override");
}

REX_TEST_CASE("an achievement batch with an invalid id registers nothing", "[xam][achievements]") {
  rex::xam::AchievementManager manager;
  const std::array batch{BuildAchievement(1, "One"), BuildAchievement(0, "Reserved"),
                         BuildAchievement(2, "Two")};
  const auto registered = manager.RegisterAchievements(batch);
  REQUIRE_FALSE(registered);
  CHECK(registered.error().category == rex::ErrorCategory::kValidation);
  CHECK(manager.ListAchievements().empty());
}

REX_TEST_CASE("a valid achievement batch registers every entry", "[xam][achievements]") {
  rex::xam::AchievementManager manager;
  const std::array batch{BuildAchievement(1, "One"), BuildAchievement(2, "Two")};
  REQUIRE(manager.RegisterAchievements(batch));
  CHECK(manager.ListAchievements().size() == 2);
}

REX_TEST_CASE("achievement notifications follow the unlock request", "[xam][achievements]") {
  rex::xam::AchievementManager manager;
  REQUIRE(manager.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  REQUIRE(manager.RegisterAchievement(BuildAchievement(2, "Two")).has_value());

  int notification_count = 0;
  auto notification_listener = manager.RegisterNotificationCallback(
      [&](const rex::xam::AchievementEvent&) { ++notification_count; });

  CHECK(manager.UnlockAchievement(1) == rex::xam::AchievementUnlockResult::kUnlocked);
  CHECK(notification_count == 0);

  CHECK(manager.UnlockAchievement(1, rex::xam::AchievementNotification::kShow) ==
        rex::xam::AchievementUnlockResult::kAlreadyUnlocked);
  CHECK(notification_count == 0);

  CHECK(manager.UnlockAchievement(2, rex::xam::AchievementNotification::kShow) ==
        rex::xam::AchievementUnlockResult::kUnlocked);
  CHECK(notification_count == 1);

  notification_listener.Reset();
  CHECK(manager.UnlockAchievement(3) == rex::xam::AchievementUnlockResult::kUnknownAchievement);
  CHECK(notification_count == 1);
}

REX_TEST_CASE("achievement unlock state persists across manager instances", "[xam][achievements]") {
  rex::test::TempFolder temp("rex_achievement_save_");
  auto save_path = temp.path / "saves" / "12345678.toml";

  rex::xam::AchievementManager first;
  REQUIRE(first.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  first.SetUnlockSavePath(save_path);
  REQUIRE(first.UnlockAchievement(1) == rex::xam::AchievementUnlockResult::kUnlocked);
  REQUIRE(std::filesystem::exists(save_path));
  auto unlock_time = first.GetUnlockTime(1);
  REQUIRE(unlock_time.has_value());

  rex::xam::AchievementManager second;
  REQUIRE(second.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  second.SetUnlockSavePath(save_path);
  second.LoadUnlockState();
  CHECK(second.IsUnlocked(1));
  CHECK(second.GetUnlockTime(1) == unlock_time);
}

REX_TEST_CASE("achievement unlock saves serialize concurrent writers", "[xam][achievements]") {
  rex::test::TempFolder temp("rex_achievement_concurrent_save_");
  auto save_path = temp.path / "saves" / "12345678.toml";

  rex::xam::AchievementManager manager;
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

  rex::xam::AchievementManager reloaded;
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

REX_TEST_CASE("runtime metadata root overrides legacy metadata discovery", "[xam][achievements]") {
  rex::test::TempFolder temp("rex_metadata_root_");
  auto game_root = temp.path / "game";
  auto local_metadata = game_root / "metadata" / "achievements.toml";
  auto parent_metadata = temp.path / "metadata" / "achievements.toml";
  auto explicit_metadata = temp.path / "custom" / "achievements.toml";
  WriteText(local_metadata, "local");
  WriteText(parent_metadata, "parent");
  WriteText(explicit_metadata, "explicit");

  {
    rex::test::LoadedTitle loaded(
        {}, false, [&] { REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(game_root)); });
    rex::kernel::TitleRuntime& runtime = loaded.runtime;
    CHECK(StateOf(runtime).FindMetadataPath("achievements.toml") == local_metadata);

    std::filesystem::remove(local_metadata);
    CHECK(StateOf(runtime).FindMetadataPath("achievements.toml") == parent_metadata);
  }

  {
    rex::test::LoadedTitle loaded({}, false, [&] {
      REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(game_root));
      REQUIRE(rex::kernel::GetTitlePaths().metadata_root.Set(temp.path / "custom"));
    });
    rex::kernel::TitleRuntime& runtime = loaded.runtime;
    CHECK(StateOf(runtime).FindMetadataPath("achievements.toml") == explicit_metadata);
    CHECK_FALSE(StateOf(runtime).FindMetadataPath("missing.toml"));
  }
}

REX_TEST_CASE("embedded metadata assets resolve by metadata relative path", "[xam][achievements]") {
  static constexpr u8 kIconBytes[] = {0x89, 0x50, 0x4E, 0x47};
  REQUIRE(rex::ppc::RegisterEmbeddedMetadataAsset("icons/custom/unit-test.png", kIconBytes,
                                                  sizeof(kIconBytes)));

  auto asset = rex::ppc::FindEmbeddedMetadataAsset(std::filesystem::path("icons") / "custom" /
                                                   "unit-test.png");
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
              "[xam][achievements][case]") {
  rex::test::TempFolder temp("rex_achievement_save_case_");
  std::filesystem::create_directories(temp.path / "Achievements");
  std::ofstream(temp.path / "Achievements" / "abcdef01.toml") << "";
  const auto found = rex::xam::ResolveUnlockSavePath(temp.path, 0xABCDEF01);
  CHECK(std::filesystem::exists(found));
}

REX_TEST_CASE("dropping the guard unsubscribes the notification callback", "[xam][achievements]") {
  rex::xam::AchievementManager manager;
  REQUIRE(manager.RegisterAchievement(BuildAchievement(1, "One")).has_value());
  int notification_count = 0;
  {
    auto notification_guard = manager.RegisterNotificationCallback(
        [&](const rex::xam::AchievementEvent&) { ++notification_count; });
  }
  CHECK(manager.UnlockAchievement(1, rex::xam::AchievementNotification::kShow) ==
        rex::xam::AchievementUnlockResult::kUnlocked);
  CHECK(notification_count == 0);
}
