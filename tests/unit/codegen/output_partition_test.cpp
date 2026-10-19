/**
 * @file        tests/unit/codegen/output_partition_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include <rex/codegen/output_partition.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

/**
 * Returns the index of the bucket holding the address, or SIZE_MAX when absent.
 *
 * @param buckets The buckets of entry indexes.
 * @param entries The entries the bucket indexes point into.
 * @param address The address to look for.
 * @return The bucket index.
 */
size_t BucketOf(const std::vector<std::vector<size_t>>& buckets,
                std::span<const FunctionSize> entries, u32 address) {
  for (size_t b = 0; b < buckets.size(); ++b) {
    for (size_t i : buckets[b]) {
      if (entries[i].address == address)
        return b;
    }
  }
  return SIZE_MAX;
}

fs::path ScratchPath(const rex::test::TempFolder& folder, std::string_view name) {
  return folder.path / fs::path(name);
}

}  // namespace

REX_TEST_CASE("File count is derived from the total size and the byte budget",
              "[codegen][output_partition]") {
  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}, {0x8200'0200, 40}};

  OutputPartition partition;
  auto buckets = partition.Assign(entries, 100);

  CHECK(buckets.size() == 2);
  CHECK(buckets[0].size() + buckets[1].size() == 3);
}

REX_TEST_CASE("New functions go to the smallest bucket", "[codegen][output_partition]") {
  std::vector<FunctionSize> entries{{0x8200'0000, 90}, {0x8200'0100, 10}, {0x8200'0200, 10}};

  OutputPartition partition;
  auto buckets = partition.Assign(entries, 55);

  REQUIRE(buckets.size() == 2);
  CHECK(BucketOf(buckets, entries, 0x8200'0000) == 0);
  CHECK(BucketOf(buckets, entries, 0x8200'0100) == 1);
  CHECK(BucketOf(buckets, entries, 0x8200'0200) == 1);
}

REX_TEST_CASE("A function stays in its bucket when an earlier function grows",
              "[codegen][output_partition]") {
  std::vector<FunctionSize> first{{0x8200'0000, 40}, {0x8200'0100, 40}, {0x8200'0200, 40}};

  OutputPartition partition;
  auto before = partition.Assign(first, 100);
  auto last_bucket_before = BucketOf(before, first, 0x8200'0200);

  std::vector<FunctionSize> second{{0x8200'0000, 90}, {0x8200'0100, 40}, {0x8200'0200, 40}};
  auto after = partition.Assign(second, 100);

  CHECK(BucketOf(after, second, 0x8200'0200) == last_bucket_before);
}

REX_TEST_CASE("The file count survives functions growing far past the budget",
              "[codegen][output_partition]") {
  std::vector<FunctionSize> first;
  first.reserve(20);
  for (u32 i = 0; i < 20; ++i) {
    first.push_back({0x8200'0000 + i * 0x100, 10});
  }

  OutputPartition partition;
  auto before = partition.Assign(first, 100);
  REQUIRE(before.size() == 2);

  std::vector<FunctionSize> second = first;
  for (auto& entry : second) {
    entry.bytes = 500;
  }

  CHECK(partition.Assign(second, 100).size() == 2);
}

REX_TEST_CASE("The file count survives a reload", "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "count.json");
  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}, {0x8200'0200, 40}};

  OutputPartition partition;
  REQUIRE(partition.Assign(entries, 100).size() == 2);
  { std::ofstream(path, std::ios::binary) << partition.Serialize(); }

  auto loaded = OutputPartition::Load(path);
  CHECK(loaded.Assign(entries, 100).size() == 2);
  fs::remove(path);
}

REX_TEST_CASE("A changed byte budget re-derives the file count", "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "budget.json");
  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}, {0x8200'0200, 40}};

  OutputPartition partition;
  REQUIRE(partition.Assign(entries, 100).size() == 2);
  { std::ofstream(path, std::ios::binary) << partition.Serialize(); }

  auto loaded = OutputPartition::Load(path);
  CHECK(loaded.Assign(entries, 40).size() == 3);
  fs::remove(path);
}

REX_TEST_CASE("A sidecar from another version is ignored", "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "other_version.json");
  {
    std::ofstream(path, std::ios::binary)
        << R"({"version":1,"file_count":7,"assignments":{"82000000":6}})";
  }

  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}};
  auto loaded = OutputPartition::Load(path);

  CHECK(loaded.Assign(entries, 100).size() == 1);
  fs::remove(path);
}

REX_TEST_CASE("A wrongly typed file_count is ignored, not thrown on",
              "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "bad_count.json");
  {
    std::ofstream(path, std::ios::binary)
        << R"({"version":2,"file_count":"two","max_file_bytes":100,"assignments":{}})";
  }

  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}, {0x8200'0200, 40}};
  auto loaded = OutputPartition::Load(path);

  CHECK(loaded.Assign(entries, 100).size() == 2);
  fs::remove(path);
}

REX_TEST_CASE("A new function does not change the file count", "[codegen][output_partition]") {
  std::vector<FunctionSize> first{{0x8200'0000, 40}, {0x8200'0100, 40}};

  OutputPartition partition;
  REQUIRE(partition.Assign(first, 100).size() == 1);

  std::vector<FunctionSize> second{{0x8200'0000, 40}, {0x8200'0080, 50}, {0x8200'0100, 40}};
  auto after = partition.Assign(second, 100);

  CHECK(after.size() == 1);
  CHECK(after[0].size() == 3);
}

REX_TEST_CASE("Buckets stay sorted by address", "[codegen][output_partition]") {
  std::vector<FunctionSize> entries{{0x8200'0000, 10}, {0x8200'0100, 10}, {0x8200'0200, 10}};

  OutputPartition partition;
  auto buckets = partition.Assign(entries, 1000);

  REQUIRE(buckets.size() == 1);
  CHECK(buckets[0] == std::vector<size_t>{0, 1, 2});
}

REX_TEST_CASE("Serialize and Load round trip preserves assignments",
              "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "roundtrip.json");
  std::vector<FunctionSize> entries{{0x8200'0000, 90}, {0x8200'0100, 10}, {0x8200'0200, 10}};

  OutputPartition partition;
  auto before = partition.Assign(entries, 55);
  { std::ofstream(path, std::ios::binary) << partition.Serialize(); }

  auto loaded = OutputPartition::Load(path);
  auto after = loaded.Assign(entries, 55);

  for (const auto& entry : entries) {
    CHECK(BucketOf(after, entries, entry.address) == BucketOf(before, entries, entry.address));
  }
  fs::remove(path);
}

REX_TEST_CASE("Functions that disappear are dropped from the serialized partition",
              "[codegen][output_partition]") {
  std::vector<FunctionSize> first{{0x8200'0000, 40}, {0x8200'0100, 40}};
  OutputPartition partition;
  partition.Assign(first, 100);

  std::vector<FunctionSize> second{{0x8200'0000, 40}};
  partition.Assign(second, 100);

  CHECK(partition.Serialize().find("82000100") == std::string::npos);
}

REX_TEST_CASE("A malformed sidecar falls back to a fresh partition",
              "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "malformed.json");
  { std::ofstream(path, std::ios::binary) << "{ this is not json"; }

  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}, {0x8200'0200, 40}};
  auto loaded = OutputPartition::Load(path);

  CHECK(loaded.Assign(entries, 100).size() == 2);
  fs::remove(path);
}

REX_TEST_CASE("A nonhex assignment key is skipped, not thrown on", "[codegen][output_partition]") {
  const rex::test::TempFolder folder("rexglue_partition_test_");
  auto path = ScratchPath(folder, "bad_key.json");
  {
    std::ofstream(path, std::ios::binary)
        << R"({"version":2,"file_count":2,"max_file_bytes":100,)"
           R"("assignments":{"nothex":1,"82000100":1,"8200x":0}})";
  }

  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}};
  auto loaded = OutputPartition::Load(path);
  auto buckets = loaded.Assign(entries, 100);

  REQUIRE(buckets.size() == 2);
  CHECK(BucketOf(buckets, entries, 0x8200'0100) == 1);
  CHECK(BucketOf(buckets, entries, 0x8200'0000) == 0);
  fs::remove(path);
}

REX_TEST_CASE("A missing sidecar falls back to a fresh partition", "[codegen][output_partition]") {
  auto loaded = OutputPartition::Load(
      ScratchPath(rex::test::TempFolder("rexglue_partition_test_"), "does_not_exist.json"));

  std::vector<FunctionSize> entries{{0x8200'0000, 40}, {0x8200'0100, 40}};
  auto buckets = loaded.Assign(entries, 100);

  REQUIRE(buckets.size() == 1);
  CHECK(buckets[0].size() == 2);
}
