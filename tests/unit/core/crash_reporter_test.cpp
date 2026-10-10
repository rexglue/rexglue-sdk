/**
 * @file        tests/unit/core/crash_reporter_test.cpp
 * @brief       Crash reporter cases, each run in a child process
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/platform/file.h>

namespace {

struct ProbeResult {
  int status = 0;
  std::string output;

  bool Reported() const { return Has("rex-crash-probe crash report"); }
  bool Has(std::string_view text) const { return output.find(text) != std::string::npos; }
};

ProbeResult RunProbe(std::string_view mode) {
  const std::filesystem::path folder = rex::platform::GetExecutableFolder();
#if REX_PLATFORM_WIN32
  const std::filesystem::path probe = folder / "rex-crash-probe.exe";
#else
  const std::filesystem::path probe = folder / "rex-crash-probe";
#endif
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / std::format("rex-crash-probe-{}.txt", mode);
  std::filesystem::remove(out);
  const std::string inner =
      std::format("\"{}\" {} > \"{}\" 2>&1", probe.string(), mode, out.string());
#if REX_PLATFORM_WIN32
  const std::string command = "\"" + inner + "\"";
#else
  const std::string command = inner;
#endif
  ProbeResult result;
  // NOLINTNEXTLINE(bugprone-command-processor): the probe runs as a child process
  result.status = std::system(command.c_str());
  std::ifstream file(out);
  std::stringstream text;
  text << file.rdbuf();
  result.output = text.str();
  CHECK_FALSE(result.Has("ThreadSanitizer"));
  return result;
}

}  // namespace

TEST_CASE("A null write is reported with the fault address", "[crash][crash_report]") {
  const auto result = RunProbe("null-write");
  CHECK(result.status != 0);
  CHECK(result.Reported());
#if REX_PLATFORM_WIN32
  CHECK(result.Has("ACCESS_VIOLATION"));
#else
  CHECK(result.Has("SIGSEGV"));
#endif
  CHECK(result.Has("fault address: 0x0000000000000000"));
  CHECK(result.Has("end of crash report"));
}

TEST_CASE("std::terminate names the exception type and message", "[crash][crash_report]") {
  const auto result = RunProbe("terminate");
  CHECK(result.status != 0);
  CHECK(result.Has("runtime_error"));
  CHECK(result.Has("probe terminate"));
}

TEST_CASE("abort is reported", "[crash][crash_report]") {
  const auto result = RunProbe("abort");
  CHECK(result.status != 0);
  CHECK(result.Reported());
  CHECK(result.Has("SIGABRT"));
}

TEST_CASE("A stack overflow is reported", "[crash][crash_report]") {
  const auto result = RunProbe("stack-overflow");
  CHECK(result.status != 0);
  CHECK(result.Reported());
#if REX_PLATFORM_WIN32
  CHECK(result.Has("STACK_OVERFLOW"));
#endif
}

TEST_CASE(
    "A GPU register write outside generated code names the block and address from the "
    "faulting thread",
    "[crash][crash_report]") {
  const auto result = RunProbe("mmio");
  CHECK(result.status != 0);
  CHECK(result.Has("GPU register write 0x7FC80010"));
  CHECK(result.Reported());
}

TEST_CASE("An access violation inside SEH_TRY is not reported", "[crash][crash_report]") {
  const auto result = RunProbe("seh");
  CHECK(result.status == 0);
  CHECK(result.Has("seh caught"));
  CHECK_FALSE(result.Reported());
}

#if !REX_PLATFORM_WIN32
TEST_CASE("A fault in recompiled code under a host frame is not caught by an outer SEH_TRY",
          "[core][seh][crash_report]") {
  const auto result = RunProbe("seh-host-frame");
  CHECK(result.status != 0);
  CHECK_FALSE(result.Has("seh caught"));
  CHECK(result.Reported());
}
#endif

#if REX_PLATFORM_WIN32
TEST_CASE("A write watch fault resumes without a report", "[crash][crash_report]") {
  const auto result = RunProbe("watch");
  CHECK(result.status == 0);
  CHECK(result.Has("watch resumed"));
  CHECK_FALSE(result.Reported());
}
#endif

TEST_CASE("A fault on a thread the SDK did not start is reported", "[crash][crash_report]") {
  const auto result = RunProbe("foreign-thread");
  CHECK(result.status != 0);
  CHECK(result.Reported());
}

TEST_CASE("A fault during another thread's report waits for the report to finish",
          "[crash][crash_report]") {
  const auto result = RunProbe("concurrent");
  CHECK(result.status != 0);
  CHECK(result.Has("probe concurrent describer"));
  CHECK(result.Has("==== end of crash report ===="));
}

TEST_CASE("The describer text is part of the report", "[crash][crash_report]") {
  const auto result = RunProbe("describer");
  CHECK(result.status != 0);
  CHECK(result.Has("probe describer 0"));
}
