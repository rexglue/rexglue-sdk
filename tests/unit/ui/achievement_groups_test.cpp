#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

#include "ui/overlay/achievement_groups.h"

namespace {

rex::system::AchievementInfo Make(uint32_t id, const char* label, uint32_t gamerscore,
                                  bool custom) {
  rex::system::AchievementInfo info;
  info.id = id;
  info.label = label;
  info.gamerscore = gamerscore;
  info.custom = custom;
  return info;
}

}  // namespace

namespace groups = rex::ui::achievement_groups;

TEST_CASE("achievement groups keep original order and sort custom", "[achievements][ui]") {
  const std::vector<rex::system::AchievementInfo> all{
      Make(3, "Zeta", 5, false),
      Make(0x10001, "Beta", 20, true),
      Make(1, "Alpha", 50, false),
      Make(0x10002, "Alpha", 20, true),
      Make(0x10003, "Aardvark", 100, true),
      Make(2, "Mid", 5, false),
  };
  const auto split = groups::Split(all);

  REQUIRE(split.original.size() == 3);
  CHECK(split.original[0].id == 3);
  CHECK(split.original[1].id == 1);
  CHECK(split.original[2].id == 2);

  REQUIRE(split.custom.size() == 3);
  CHECK(split.custom[0].id == 0x10002);  // lowest gamerscore first, tie broken by label
  CHECK(split.custom[1].id == 0x10001);
  CHECK(split.custom[2].id == 0x10003);
}

TEST_CASE("achievement groups break full ties by id", "[achievements][ui]") {
  const auto split = groups::Split({Make(9, "Same", 10, true), Make(4, "Same", 10, true)});
  REQUIRE(split.custom.size() == 2);
  CHECK(split.custom[0].id == 4);
  CHECK(split.custom[1].id == 9);
}

TEST_CASE("achievement groups without custom entries leave the catalog alone",
          "[achievements][ui]") {
  const auto split = groups::Split({Make(2, "B", 1, false), Make(1, "A", 9, false)});
  CHECK(split.custom.empty());
  REQUIRE(split.original.size() == 2);
  CHECK(split.original[0].id == 2);
  CHECK(groups::Split({}).original.empty());
}

TEST_CASE("achievement group totals count each group separately", "[achievements][ui]") {
  const auto split = groups::Split({Make(1, "A", 10, false), Make(2, "B", 20, false),
                                    Make(0x10001, "C", 30, true), Make(0x10002, "D", 40, true)});
  const std::unordered_set<uint32_t> unlocked{2, 0x10002};
  const auto is_unlocked = [&](uint32_t id) { return unlocked.contains(id); };

  const auto original = groups::Summarize(split.original, is_unlocked);
  CHECK(original.unlocked == 1);
  CHECK(original.total == 2);
  CHECK(original.earned_gamerscore == 20);
  CHECK(original.total_gamerscore == 30);

  const auto custom = groups::Summarize(split.custom, is_unlocked);
  CHECK(custom.unlocked == 1);
  CHECK(custom.total == 2);
  CHECK(custom.earned_gamerscore == 40);
  CHECK(custom.total_gamerscore == 70);

  const auto empty = groups::Summarize({}, is_unlocked);
  CHECK(empty.total == 0);
}
