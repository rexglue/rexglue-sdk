/**
 * @file        achievement_manager_test.cpp
 * @brief       Unit tests for achievement metadata, persistence, and events.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/runtime.h>
#include <rex/embedded_metadata.h>
#include <rex/system/achievement_manager.h>

namespace {

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view name) {
    static std::atomic<uint64_t> next_id{0};
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "_" + std::to_string(suffix) + "_" + std::to_string(next_id++));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

rex::system::AchievementInfo MakeAchievement(uint32_t id, std::string label) {
  rex::system::AchievementInfo info;
  info.id = id;
  info.label = std::move(label);
  info.description = "Unlocked";
  info.unachieved_description = "Locked";
  info.image_id = id + 100;
  info.gamerscore = 10;
  return info;
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  REQUIRE(file);
  file << content;
}

}  // namespace

TEST_CASE("achievement metadata layers embedded, TOML, and recomp entries", "[achievements]") {
  TempDirectory temp("rex_achievement_metadata");
  auto metadata_path = temp.path() / "achievements.toml";
  WriteFile(metadata_path, R"(
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

  rex::system::AchievementManager manager;
  manager.ReplaceAchievements({MakeAchievement(1, "Embedded")});
  REQUIRE(manager.LoadMetadataFile(metadata_path));

  auto first = manager.FindAchievement(1);
  REQUIRE(first);
  CHECK(first->label == "Metadata override");
  CHECK(first->icon_path == "custom/one.png");
  CHECK(manager.ListAchievements().size() == 2);

  REQUIRE(manager.RegisterAchievement(MakeAchievement(2, "Recomp override")));
  auto second = manager.FindAchievement(2);
  REQUIRE(second);
  CHECK(second->label == "Recomp override");
}

TEST_CASE("achievement notifications are explicit and replaceable", "[achievements]") {
  rex::system::AchievementManager manager;
  manager.RegisterAchievement(MakeAchievement(1, "One"));
  manager.RegisterAchievement(MakeAchievement(2, "Two"));

  int unlock_count = 0;
  int notification_count = 0;
  auto unlock_listener =
      manager.RegisterUnlockCallback([&](const rex::system::AchievementEvent&) { ++unlock_count; });
  auto notification_listener = manager.RegisterNotificationCallback(
      [&](const rex::system::AchievementEvent&) { ++notification_count; });

  CHECK(manager.UnlockAchievement(1) == rex::system::AchievementUnlockResult::kUnlocked);
  CHECK(unlock_count == 1);
  CHECK(notification_count == 0);

  CHECK(manager.ShowAchievementNotification(1));
  CHECK(notification_count == 1);

  CHECK(manager.UnlockAchievement(1, rex::system::AchievementNotification::kShow) ==
        rex::system::AchievementUnlockResult::kAlreadyUnlocked);
  CHECK(notification_count == 1);

  CHECK(manager.UnlockAchievement(2, rex::system::AchievementNotification::kShow) ==
        rex::system::AchievementUnlockResult::kUnlocked);
  CHECK(unlock_count == 2);
  CHECK(notification_count == 2);

  manager.UnregisterCallback(unlock_listener);
  manager.UnregisterCallback(notification_listener);
  CHECK_FALSE(manager.ShowAchievementNotification(99));
}

TEST_CASE("achievement unlock state persists across manager instances", "[achievements]") {
  TempDirectory temp("rex_achievement_save");
  auto save_path = temp.path() / "saves" / "12345678.toml";

  rex::system::AchievementManager first;
  first.RegisterAchievement(MakeAchievement(1, "One"));
  first.SetUnlockSavePath(save_path);
  REQUIRE(first.UnlockAchievement(1) == rex::system::AchievementUnlockResult::kUnlocked);
  REQUIRE(std::filesystem::exists(save_path));
  auto unlock_time = first.GetUnlockTime(1);
  REQUIRE(unlock_time != 0);

  rex::system::AchievementManager second;
  second.RegisterAchievement(MakeAchievement(1, "One"));
  second.SetUnlockSavePath(save_path);
  second.LoadUnlockState();
  CHECK(second.IsUnlocked(1));
  CHECK(second.GetUnlockTime(1) == unlock_time);
}

TEST_CASE("achievement unlock saves serialize concurrent writers", "[achievements]") {
  TempDirectory temp("rex_achievement_concurrent_save");
  auto save_path = temp.path() / "saves" / "12345678.toml";

  rex::system::AchievementManager manager;
  manager.SetUnlockSavePath(save_path);
  for (uint32_t id = 1; id <= 8; ++id) {
    manager.RegisterAchievement(MakeAchievement(id, "Achievement " + std::to_string(id)));
  }

  std::vector<std::thread> threads;
  for (uint32_t id = 1; id <= 8; ++id) {
    threads.emplace_back([&manager, id]() { (void)manager.UnlockAchievement(id); });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  rex::system::AchievementManager reloaded;
  reloaded.SetUnlockSavePath(save_path);
  for (uint32_t id = 1; id <= 8; ++id) {
    reloaded.RegisterAchievement(MakeAchievement(id, "Achievement " + std::to_string(id)));
  }
  reloaded.LoadUnlockState();
  for (uint32_t id = 1; id <= 8; ++id) {
    CHECK(reloaded.IsUnlocked(id));
  }
}

TEST_CASE("runtime metadata root overrides legacy metadata discovery", "[achievements]") {
  TempDirectory temp("rex_metadata_root");
  auto game_root = temp.path() / "game";
  auto local_metadata = game_root / "metadata" / "achievements.toml";
  auto parent_metadata = temp.path() / "metadata" / "achievements.toml";
  auto explicit_metadata = temp.path() / "custom" / "achievements.toml";
  WriteFile(local_metadata, "local");
  WriteFile(parent_metadata, "parent");
  WriteFile(explicit_metadata, "explicit");

  {
    rex::Runtime runtime(game_root);
    REQUIRE(runtime.FindMetadataPath("achievements.toml"));
    CHECK(*runtime.FindMetadataPath("achievements.toml") == local_metadata);

    std::filesystem::remove(local_metadata);
    REQUIRE(runtime.FindMetadataPath("achievements.toml"));
    CHECK(*runtime.FindMetadataPath("achievements.toml") == parent_metadata);
  }

  {
    rex::Runtime runtime(game_root, {}, {}, {}, temp.path() / "custom");
    REQUIRE(runtime.FindMetadataPath("achievements.toml"));
    CHECK(*runtime.FindMetadataPath("achievements.toml") == explicit_metadata);
    CHECK_FALSE(runtime.FindMetadataPath("missing.toml"));
  }
}

TEST_CASE("embedded metadata assets resolve by metadata-relative path", "[achievements]") {
  static constexpr std::uint8_t kIconBytes[] = {0x89, 0x50, 0x4E, 0x47};
  REQUIRE(rex::RegisterEmbeddedMetadataAsset("icons/custom/unit-test.png", kIconBytes,
                                             sizeof(kIconBytes)));

  auto asset =
      rex::FindEmbeddedMetadataAsset(std::filesystem::path("icons") / "custom" / "unit-test.png");
  REQUIRE(asset);
  REQUIRE(asset->bytes.size() == sizeof(kIconBytes));
  CHECK(asset->bytes[0] == 0x89);
  CHECK(asset->bytes[1] == 0x50);
  CHECK(asset->bytes[2] == 0x4E);
  CHECK(asset->bytes[3] == 0x47);
}

TEST_CASE("achievement custom marker is separate from flags", "[achievements]") {
  TempDirectory temp("rex_achievement_custom");
  auto metadata_path = temp.path() / "achievements.toml";
  WriteFile(metadata_path, R"(
[[achievements]]
id = 7
label = "Extra"
gamerscore = 5
flags = 3
custom = true
)");

  rex::system::AchievementManager manager;
  manager.ReplaceAchievements({MakeAchievement(1, "Embedded")});
  REQUIRE(manager.LoadMetadataFile(metadata_path));

  CHECK_FALSE(manager.FindAchievement(1)->custom);
  const auto extra = manager.FindAchievement(7);
  REQUIRE(extra);
  CHECK(extra->custom);
  CHECK(extra->flags == 3);
}
