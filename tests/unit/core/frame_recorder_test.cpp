/**
 * @file        tests/unit/core/frame_recorder_test.cpp
 * @brief       Unit tests for the per-frame time breakdown (rex/perf/frame_recorder.h)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/perf/frame_recorder.h>

using namespace rex::perf;
using namespace std::chrono_literals;

namespace {

uint32_t Index(FrameZone zone) {
  return static_cast<uint32_t>(zone);
}

// The recorder is one per process: start each test from nothing, at `level`.
struct Fresh {
  explicit Fresh(uint32_t level, uint32_t capacity = 0) {
    rex_frame_recorder_set_level(0);
    rex_frame_recorder_reset(capacity);
    rex_frame_recorder_set_level(level);
  }
  ~Fresh() {
    rex_frame_recorder_set_level(0);
    rex_frame_recorder_reset(0);
  }
};

FrameSummary Summary(uint32_t last_frames = 0) {
  FrameSummary summary;
  rex_frame_recorder_summary(last_frames, &summary);
  return summary;
}

// A CSV file read back as a header and rows of cells.
struct Csv {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;

  size_t Column(const std::string& name) const {
    for (size_t i = 0; i < header.size(); ++i) {
      if (header[i] == name)
        return i;
    }
    FAIL("no column " << name);
    return 0;
  }
  double Number(size_t row, const std::string& name) const {
    return std::stod(rows[row][Column(name)]);
  }
};

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> cells;
  std::stringstream stream(line);
  std::string cell;
  while (std::getline(stream, cell, ','))
    cells.push_back(cell);
  return cells;
}

Csv ReadCsv(const std::filesystem::path& path) {
  Csv csv;
  std::ifstream file(path);
  std::string line;
  if (std::getline(file, line))
    csv.header = Split(line);
  while (std::getline(file, line))
    csv.rows.push_back(Split(line));
  return csv;
}

// A file under the temporary folder, removed when the test ends.
struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const char* name) : path(std::filesystem::temp_directory_path() / name) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  std::string Utf8() const {
    // std::u8string normally, std::string under the SDK's -fno-char8_t.
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
  }
};

Csv Dump(const char* name) {
  TempFile file(name);
  REQUIRE(rex_frame_recorder_dump(file.Utf8().c_str(), 1) > 0);
  return ReadCsv(file.path);
}

}  // namespace

TEST_CASE("frame zone names", "[perf][frame_recorder]") {
  SECTION("every zone has its own name") {
    std::set<std::string> names;
    for (uint32_t zone = 0; zone < kFrameZoneCount; ++zone) {
      const std::string name = rex_frame_zone_name(zone);
      CHECK_FALSE(name.empty());
      names.insert(name);
    }
    CHECK(names.size() == kFrameZoneCount);
  }

  SECTION("a zone out of range has no name") {
    CHECK(std::string(rex_frame_zone_name(kFrameZoneCount)).empty());
  }
}

TEST_CASE("frame recorder levels", "[perf][frame_recorder]") {
  SECTION("level 0 records nothing") {
    Fresh fresh(0);
    for (int frame = 0; frame < 3; ++frame) {
      {
        REX_FRAME_ZONE(kDraw);
      }
      rex_frame_recorder_end_frame();
    }
    CHECK(Summary().frames == 0);
    TempFile file("rex-frame-recorder-off.csv");
    CHECK(rex_frame_recorder_dump(file.Utf8().c_str(), 1) == 0);
    CHECK_FALSE(std::filesystem::exists(file.path));
  }

  SECTION("level 1 leaves out the sections inside a draw") {
    Fresh fresh(1);
    rex_frame_recorder_end_frame();
    {
      REX_FRAME_ZONE(kDraw);
      REX_FRAME_ZONE_DETAIL(kDrawTextures);
    }
    rex_frame_recorder_end_frame();
    const FrameSummary summary = Summary();
    CHECK(summary.zone_calls_mean[Index(FrameZone::kDraw)] == 1.0f);
    CHECK(summary.zone_calls_mean[Index(FrameZone::kDrawTextures)] == 0.0f);
  }

  SECTION("level 2 records them") {
    Fresh fresh(2);
    rex_frame_recorder_end_frame();
    {
      REX_FRAME_ZONE(kDraw);
      REX_FRAME_ZONE_DETAIL(kDrawTextures);
    }
    rex_frame_recorder_end_frame();
    CHECK(Summary().zone_calls_mean[Index(FrameZone::kDrawTextures)] == 1.0f);
  }

  SECTION("a level above 2 is treated as 2") {
    Fresh fresh(7);
    CHECK(FrameRecorderLevel() == 2);
  }

  SECTION("the time it was off is not recorded as a frame") {
    Fresh fresh(1);
    rex_frame_recorder_end_frame();
    rex_frame_recorder_end_frame();
    rex_frame_recorder_set_level(0);
    rex_frame_recorder_end_frame();
    std::this_thread::sleep_for(60ms);
    rex_frame_recorder_set_level(1);
    rex_frame_recorder_end_frame();
    rex_frame_recorder_end_frame();
    const FrameSummary summary = Summary();
    CHECK(summary.frames == 2);
    CHECK(summary.frame_ms_max < 50.0f);
  }
}

TEST_CASE("frame recorder timing", "[perf][frame_recorder]") {
  Fresh fresh(1);
  rex_frame_recorder_end_frame();  // the first call only marks the start

  SECTION("a frame gets its length and the time in each section") {
    {
      REX_FRAME_ZONE(kDraw);
      std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(10ms);
    rex_frame_recorder_end_frame();
    const FrameSummary summary = Summary();
    const float draw = summary.zone_ms_mean[Index(FrameZone::kDraw)];
    REQUIRE(summary.frames == 1);
    CHECK(summary.frame_ms_mean >= 29.0f);
    CHECK(draw >= 19.0f);
    CHECK(draw <= summary.frame_ms_mean - 9.0f);
    CHECK(summary.zone_calls_mean[Index(FrameZone::kDraw)] == 1.0f);
    // The 10 ms outside the section is the frame thread's unaccounted time.
    CHECK(summary.other_ms_mean >= 9.0f);
    CHECK(summary.other_ms_mean <= summary.frame_ms_mean - draw + 0.5f);
  }

  SECTION("a nested section is not counted in the outer one") {
    {
      REX_FRAME_ZONE(kDraw);
      std::this_thread::sleep_for(15ms);
      {
        REX_FRAME_ZONE(kFenceWait);
        std::this_thread::sleep_for(15ms);
      }
    }
    rex_frame_recorder_end_frame();
    const FrameSummary summary = Summary();
    const float draw = summary.zone_ms_mean[Index(FrameZone::kDraw)];
    const float wait = summary.zone_ms_mean[Index(FrameZone::kFenceWait)];
    CHECK(draw >= 14.0f);
    CHECK(wait >= 14.0f);
    CHECK(draw + wait <= summary.frame_ms_mean + 0.5f);
  }

  SECTION("the outer section goes on counting after the nested one ends") {
    {
      REX_FRAME_ZONE(kDraw);
      { REX_FRAME_ZONE(kFenceWait); }
      std::this_thread::sleep_for(15ms);
    }
    rex_frame_recorder_end_frame();
    CHECK(Summary().zone_ms_mean[Index(FrameZone::kDraw)] >= 14.0f);
  }

  SECTION("every pass through a section is counted") {
    for (int i = 0; i < 250; ++i) {
      REX_FRAME_ZONE(kDraw);
    }
    rex_frame_recorder_end_frame();
    CHECK(Summary().zone_calls_mean[Index(FrameZone::kDraw)] == 250.0f);
  }

  SECTION("a section on another thread is recorded, but is not the frame thread's time") {
    std::thread([] {
      REX_FRAME_ZONE(kPresent);
      std::this_thread::sleep_for(15ms);
    }).join();
    rex_frame_recorder_end_frame();
    const FrameSummary summary = Summary();
    CHECK(summary.zone_calls_mean[Index(FrameZone::kPresent)] == 1.0f);
    CHECK(summary.zone_ms_mean[Index(FrameZone::kPresent)] >= 14.0f);
    CHECK(summary.other_ms_mean >= summary.frame_ms_mean - 0.5f);
  }

  SECTION("a section belongs to the frame in which it ends") {
    {
      REX_FRAME_ZONE(kDraw);
      rex_frame_recorder_end_frame();
    }
    rex_frame_recorder_end_frame();
    const Csv csv = Dump("rex-frame-recorder-ends.csv");
    REQUIRE(csv.rows.size() == 2);
    CHECK(csv.Number(0, "draw_n") == 0);
    CHECK(csv.Number(1, "draw_n") == 1);
  }
}

TEST_CASE("frame recorder buffer", "[perf][frame_recorder]") {
  SECTION("keeps only the newest frames") {
    Fresh fresh(1, 4);
    for (int frame = 0; frame < 11; ++frame)
      rex_frame_recorder_end_frame();
    CHECK(Summary().frames == 4);
    const Csv csv = Dump("rex-frame-recorder-ring.csv");
    REQUIRE(csv.rows.size() == 4);
    // Eleven calls make ten frames, numbered 0 to 9.
    CHECK(csv.Number(0, "frame") == 6);
    CHECK(csv.Number(3, "frame") == 9);
  }

  SECTION("a summary can cover only the last frames") {
    Fresh fresh(1);
    for (int frame = 0; frame < 9; ++frame)
      rex_frame_recorder_end_frame();
    CHECK(Summary(3).frames == 3);
    CHECK(Summary(100).frames == 8);
  }

  SECTION("reset forgets the frames") {
    Fresh fresh(1);
    for (int frame = 0; frame < 5; ++frame)
      rex_frame_recorder_end_frame();
    rex_frame_recorder_reset(0);
    CHECK(Summary().frames == 0);
  }
}

TEST_CASE("frame recorder summary", "[perf][frame_recorder]") {
  Fresh fresh(1);
  rex_frame_recorder_end_frame();
  for (int frame = 0; frame < 9; ++frame) {
    std::this_thread::sleep_for(2ms);
    rex_frame_recorder_end_frame();
  }
  std::this_thread::sleep_for(40ms);
  rex_frame_recorder_end_frame();
  const FrameSummary summary = Summary();

  SECTION("orders the percentiles") {
    REQUIRE(summary.frames == 10);
    CHECK(summary.frame_ms_p50 <= summary.frame_ms_p95);
    CHECK(summary.frame_ms_p95 <= summary.frame_ms_p99);
    CHECK(summary.frame_ms_p99 <= summary.frame_ms_max);
  }

  SECTION("the one long frame is the maximum, not the median") {
    CHECK(summary.frame_ms_max >= 39.0f);
    CHECK(summary.frame_ms_p50 < 20.0f);
  }
}

TEST_CASE("frame recorder CSV", "[perf][frame_recorder]") {
  Fresh fresh(1);
  rex_frame_recorder_end_frame();
  for (int frame = 0; frame < 6; ++frame) {
    {
      REX_FRAME_ZONE(kDraw);
      std::this_thread::sleep_for(frame == 3 ? 50ms : 3ms);
    }
    rex_frame_recorder_end_frame();
  }

  SECTION("has a column of time and a column of counts per zone") {
    const Csv csv = Dump("rex-frame-recorder-columns.csv");
    CHECK(csv.header.size() == 5 + 2 * kFrameZoneCount);
    CHECK(csv.header[0] == "frame");
    CHECK(csv.Column("draw_ms") > 0);
    CHECK(csv.Column("draw_shared_memory_n") == csv.header.size() - 1);
    REQUIRE(csv.rows.size() == 6);
    for (const auto& row : csv.rows)
      CHECK(row.size() == csv.header.size());
  }

  SECTION("gives times in milliseconds") {
    const Csv csv = Dump("rex-frame-recorder-units.csv");
    REQUIRE(csv.rows.size() == 6);
    CHECK(csv.Number(3, "frame_ms") >= 49.0);
    CHECK(csv.Number(3, "draw_ms") >= 49.0);
    CHECK(csv.Number(3, "draw_ms") <= csv.Number(3, "frame_ms"));
    CHECK(csv.Number(5, "end_ms") > csv.Number(0, "end_ms"));
  }

  SECTION("flags a frame over twice the median as a spike") {
    const Csv csv = Dump("rex-frame-recorder-spike.csv");
    REQUIRE(csv.rows.size() == 6);
    for (size_t row = 0; row < csv.rows.size(); ++row) {
      CHECK(csv.Number(row, "spike") == (row == 3 ? 1 : 0));
    }
  }

  SECTION("can write the file on its own thread") {
    TempFile file("rex-frame-recorder-async.csv");
    REQUIRE(rex_frame_recorder_dump(file.Utf8().c_str(), 0) == 6);
    Csv csv;
    for (int attempt = 0; attempt < 500 && csv.rows.size() < 6; ++attempt) {
      std::this_thread::sleep_for(10ms);
      csv = ReadCsv(file.path);
    }
    CHECK(csv.rows.size() == 6);
  }

  SECTION("refuses an empty path") {
    CHECK(rex_frame_recorder_dump("", 1) == 0);
    CHECK(rex_frame_recorder_dump(nullptr, 1) == 0);
  }
}

TEST_CASE("frame recorder with many threads", "[perf][frame_recorder]") {
  SECTION("loses no section while frames end and files are written") {
    constexpr int kThreads = 4;
    constexpr int kPasses = 20000;
    Fresh fresh(2);
    rex_frame_recorder_end_frame();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([] {
        for (int i = 0; i < kPasses; ++i) {
          REX_FRAME_ZONE(kDraw);
          REX_FRAME_ZONE_DETAIL(kDrawPipeline);
        }
      });
    }
    TempFile scratch("rex-frame-recorder-threads-scratch.csv");
    for (int frame = 0; frame < 200; ++frame) {
      rex_frame_recorder_end_frame();
      if (frame % 50 == 49)
        rex_frame_recorder_dump(scratch.Utf8().c_str(), 1);
    }
    for (std::thread& thread : threads)
      thread.join();
    rex_frame_recorder_end_frame();

    const Csv csv = Dump("rex-frame-recorder-threads.csv");
    double draws = 0, lookups = 0;
    for (size_t row = 0; row < csv.rows.size(); ++row) {
      draws += csv.Number(row, "draw_n");
      lookups += csv.Number(row, "draw_pipeline_n");
    }
    CHECK(draws == kThreads * kPasses);
    CHECK(lookups == kThreads * kPasses);
  }

  SECTION("a thread that exits hands its totals in first") {
    Fresh fresh(1);
    rex_frame_recorder_end_frame();
    for (int t = 0; t < 8; ++t) {
      std::thread([] { REX_FRAME_ZONE(kCopy); }).join();
    }
    rex_frame_recorder_end_frame();
    CHECK(Summary().zone_calls_mean[Index(FrameZone::kCopy)] == 8.0f);
  }
}
