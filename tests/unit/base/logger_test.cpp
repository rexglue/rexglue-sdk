/**
 * @file        tests/unit/base/logger_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>

#include "base/app_harness.h"

REXLOG_CATEGORY(logtest, "Logger test category")
REXLOG_TABLE(kLogTestTable, logtest)

namespace {

using rex::Logger;
using rex::test::HarnessApp;

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::vector<std::string> Texts(const rex::log::LogCaptureSink& sink) {
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  std::vector<std::string> out;
  for (const auto& entry : entries) {
    out.push_back(entry.text);
  }
  return out;
}

class LineSink final : public rex::log::ILogSink {
 public:
  void Write(const rex::log::Message& message) override { lines.emplace_back(message.text); }
  void Flush() override {}
  std::vector<std::string> lines;
};

std::vector<std::filesystem::path> LogFiles(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder / "logs", ec)) {
    files.push_back(entry.path());
  }
  std::ranges::sort(files);
  return files;
}

void ResetLogger() {
  HarnessApp app;
  REQUIRE(app.Start());
  app.Finish();
}

#if !defined(_WIN32)
size_t CountInStreams(const std::string& needle, const std::filesystem::path& folder) {
  size_t count = 0;
  for (const char* name : {"out.txt", "err.txt"}) {
    const std::string text = ReadFile(folder / name);
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
      ++count;
    }
  }
  return count;
}
#endif

}  // namespace

REX_TEST_CASE("line before any app reaches the file", "[base][logger]") {
  ResetLogger();
  REXLOG_INFO("line written before anything opened the file");
  HarnessApp app;
  REQUIRE(app.Start());
  REXLOG_INFO("line written after the file opened");
  app.Finish();
  const auto files = LogFiles(app.folder);
  REQUIRE(files.size() == 1);
  REQUIRE(files[0].filename().string().starts_with("harness_"));
  const std::string text = ReadFile(files[0]);
  REQUIRE(text.find("line written before anything opened the file") != std::string::npos);
  REQUIRE(text.find("line written after the file opened") != std::string::npos);
}

REX_TEST_CASE("dropping the guard removes the sink", "[base][logger]") {
  ResetLogger();
  HarnessApp app;
  REQUIRE(app.Start());
  rex::log::LogCaptureSink sink;
  {
    auto sink_guard = REX_LOGGER().AddSink(sink);
    REXLOG_INFO("while subscribed");
  }
  REXLOG_INFO("after the guard");
  app.Finish();
  const auto texts = Texts(sink);
  CHECK(std::ranges::contains(texts, std::string("while subscribed")));
  CHECK_FALSE(std::ranges::contains(texts, std::string("after the guard")));
}

REX_TEST_CASE("late sink gets the last 4096 lines", "[base][logger]") {
  ResetLogger();
  HarnessApp app;
  REQUIRE(app.Start());
  for (int i = 0; i < 5000; ++i) {
    REXLOG_INFO("bulk {}", i);
  }
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REXLOG_INFO("line after the sink joined");
  sink_guard.Reset();
  app.Finish();
  const auto texts = Texts(sink);
  REQUIRE(texts.size() == rex::log::LogCaptureSink::kCapacity);
  REQUIRE(texts.front() == "bulk 905");
  REQUIRE(texts[4094] == "bulk 4999");
  REQUIRE(texts.back() == "line after the sink joined");
}

REX_TEST_CASE("file off releases kept lines", "[base][logger]") {
  ResetLogger();
  REQUIRE(REX_LOGGER().file.Set(false));
  REXLOG_INFO("line that nothing keeps");
  HarnessApp app;
  REQUIRE(app.Start());
  REQUIRE(LogFiles(app.folder).empty());
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REXLOG_INFO("line after the sink joined");
  sink_guard.Reset();
  app.Finish();
  const auto texts = Texts(sink);
  REQUIRE(texts.size() == 1);
  REQUIRE(texts[0] == "line after the sink joined");
}

REX_TEST_CASE("category level follows its setting", "[base][logger]") {
  ResetLogger();
  Logger& logger = REX_LOGGER();
  REQUIRE(logger.AddCategories(rex::log::kLogTestTable));
  rex::log::LogCaptureSink sink;
  auto sink_guard = logger.AddSink(sink);
  REXLOG_CAT_INFO(::rex::log::logtest, "info before the level");
  REQUIRE(REX_SETTINGS().SetText("log.categories.logtest", "error"));
  REXLOG_CAT_INFO(::rex::log::logtest, "info after the level");
  REXLOG_CAT_ERROR(::rex::log::logtest, "error after the level");
  REQUIRE(REX_SETTINGS().SetText("log.categories.logtest", ""));
  REXLOG_CAT_INFO(::rex::log::logtest, "info after the reset");
  sink_guard.Reset();
  logger.RemoveCategories(rex::log::kLogTestTable);

  const auto texts = Texts(sink);
  REQUIRE(std::ranges::find(texts, "info before the level") != texts.end());
  REQUIRE(std::ranges::find(texts, "info after the level") == texts.end());
  REQUIRE(std::ranges::find(texts, "error after the level") != texts.end());
  REQUIRE(std::ranges::find(texts, "info after the reset") != texts.end());
  REQUIRE_FALSE(REX_SETTINGS().Find("log.categories.logtest"));
  ResetLogger();
}

REX_TEST_CASE("log.level sets the level of categories without their own", "[base][logger]") {
  ResetLogger();
  Logger& logger = REX_LOGGER();
  REQUIRE(logger.AddCategories(rex::log::kLogTestTable));
  rex::log::LogCaptureSink sink;
  auto sink_guard = logger.AddSink(sink);
  REQUIRE(logger.level.Set("warn"));
  REXLOG_CAT_INFO(::rex::log::logtest, "dropped by the default level");
  REXLOG_CAT_WARN(::rex::log::logtest, "kept by the default level");
  sink_guard.Reset();
  logger.RemoveCategories(rex::log::kLogTestTable);
  const auto texts = Texts(sink);
  REQUIRE(std::ranges::find(texts, "dropped by the default level") == texts.end());
  REQUIRE(std::ranges::find(texts, "kept by the default level") != texts.end());
  ResetLogger();
}

REX_TEST_CASE("a category added twice is refused", "[base][logger]") {
  ResetLogger();
  Logger& logger = REX_LOGGER();
  REQUIRE(logger.AddCategories(rex::log::kLogTestTable));
  REQUIRE_FALSE(logger.AddCategories(rex::log::kLogTestTable));
  logger.RemoveCategories(rex::log::kLogTestTable);
  REQUIRE(logger.AddCategories(rex::log::kLogTestTable));
  logger.RemoveCategories(rex::log::kLogTestTable);
}

REX_TEST_CASE("an absolute file_name picks the log file", "[base][logger]") {
  ResetLogger();
  rex::test::TempFolder folder("rex_logger_");
  const auto named = folder.path / "explicit.log";
  REQUIRE(REX_LOGGER().file_name.Set(named.string()));
  HarnessApp app;
  REQUIRE(app.Start());
  REXLOG_INFO("line for the explicit file");
  app.Finish();
  REQUIRE(LogFiles(app.folder).empty());
  REQUIRE(ReadFile(named).find("line for the explicit file") != std::string::npos);
}

REX_TEST_CASE("folder_budget prunes older runs", "[base][logger]") {
  ResetLogger();
  rex::test::TempFolder folder("rex_logger_");
  {
    HarnessApp first(folder.path);
    REQUIRE(first.Start());
    REXLOG_INFO("line for the first run");
    first.Finish();
  }
  REQUIRE(LogFiles(folder.path).size() == 1);
  const auto first_file = LogFiles(folder.path)[0];
  REQUIRE(first_file.filename() == "harness_001.log");
  {
    HarnessApp second(folder.path);
    REQUIRE(second.Start({"--log.folder_budget=1"}));
    second.Finish();
  }
  const auto files = LogFiles(folder.path);
  REQUIRE(files.size() == 1);
  REQUIRE(files[0].filename() == "harness_002.log");
}

REX_TEST_CASE("the buffer before the file keeps 65536 lines and counts the rest",
              "[base][logger]") {
  ResetLogger();
  for (int i = 0; i < 70000; ++i) {
    REXLOG_INFO("cap {}", i);
  }
  LineSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  sink_guard.Reset();
  REQUIRE(sink.lines.size() == 65537);
  REQUIRE(sink.lines[0].starts_with("4464 earlier lines were discarded"));
  REQUIRE(sink.lines[1] == "cap 4464");
  REQUIRE(sink.lines.back() == "cap 69999");
  ResetLogger();
}

#if !defined(_WIN32)
REX_TEST_CASE("one line per sink on Linux", "[base][logger]") {
  ResetLogger();
  rex::test::TempFolder folder("rex_logger_");
  HarnessApp app;
  REQUIRE(app.Start());
  std::fflush(stdout);
  std::fflush(stderr);
  const int saved_out = dup(STDOUT_FILENO);
  const int saved_err = dup(STDERR_FILENO);
  REQUIRE(saved_out >= 0);
  REQUIRE(saved_err >= 0);
  std::FILE* out = std::freopen((folder.path / "out.txt").c_str(), "w", stdout);
  std::FILE* err = std::freopen((folder.path / "err.txt").c_str(), "w", stderr);
  REQUIRE(out != nullptr);
  REQUIRE(err != nullptr);
  REXLOG_INFO("line that must appear once");
  REX_LOGGER().Flush();
  std::fflush(stdout);
  std::fflush(stderr);
  dup2(saved_out, STDOUT_FILENO);
  dup2(saved_err, STDERR_FILENO);
  close(saved_out);
  close(saved_err);
  clearerr(stdout);
  clearerr(stderr);
  app.Finish();
  REQUIRE(CountInStreams("line that must appear once", folder.path) == 1);
}
#endif

REX_TEST_CASE("the default pattern writes the timestamp, level letter, category and thread",
              "[base][logger]") {
  ResetLogger();
  REQUIRE(REX_LOGGER().level.Set("trace"));
  HarnessApp app;
  REQUIRE(app.Start());
  REXLOG_TRACE("pattern line");
  REXLOG_DEV("pattern line");
  REXLOG_DEBUG("pattern line");
  REXLOG_INFO("pattern line");
  REXLOG_WARN("pattern line");
  REXLOG_ERROR("pattern line");
  REXLOG_CRITICAL("pattern line");
  app.Finish();
  const auto files = LogFiles(app.folder);
  REQUIRE(files.size() == 1);
  const std::string text = ReadFile(files[0]);
  const std::regex line(
      "(\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3}) ([TVDIWEC]) core       t\\d{5} "
      "pattern line\n");
  std::string letters;
  for (std::sregex_iterator it(text.begin(), text.end(), line), end; it != end; ++it) {
    letters += (*it)[2].str();
  }
  CHECK(letters == "TVDIWEC");
}

REX_TEST_CASE("a custom pattern changes the file line", "[base][logger]") {
  ResetLogger();
  REQUIRE(REX_LOGGER().pattern.Set("%Q|%n|%v"));
  HarnessApp app;
  REQUIRE(app.Start());
  REXLOG_INFO("custom pattern line");
  app.Finish();
  const auto files = LogFiles(app.folder);
  REQUIRE(files.size() == 1);
  CHECK(ReadFile(files[0]).find("\nI|core|custom pattern line\n") != std::string::npos);
}

REX_TEST_CASE("a pattern with an unknown flag logs an error and uses the default",
              "[base][logger]") {
  ResetLogger();
  REQUIRE(REX_LOGGER().pattern.Set("%k %v"));
  HarnessApp app;
  REQUIRE(app.Start());
  REXLOG_INFO("line under the fallback");
  app.Finish();
  const auto files = LogFiles(app.folder);
  REQUIRE(files.size() == 1);
  const std::string text = ReadFile(files[0]);
  CHECK(text.find("log.pattern '%k %v' is not used") != std::string::npos);
  CHECK(std::regex_search(text, std::regex(" I core       t\\d{5} line under the fallback\n")));
}

REX_TEST_CASE("a positive flush_interval writes the file without a flush call", "[base][logger]") {
  ResetLogger();
  REQUIRE(REX_LOGGER().flush_interval.Set(1));
  HarnessApp app;
  REQUIRE(app.Start());
  REXLOG_INFO("line the periodic flush writes");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool found = false;
  while (!found && std::chrono::steady_clock::now() < deadline) {
    const auto files = LogFiles(app.folder);
    found = !files.empty() &&
            ReadFile(files[0]).find("line the periodic flush writes") != std::string::npos;
    if (!found) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  CHECK(found);
  app.Finish();
}
