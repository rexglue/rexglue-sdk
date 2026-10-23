/**
 * @file        tests/unit/core/relaunch_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/env.h>
#include <rex/platform/file.h>
#include <rex/platform/process.h>

#if REX_PLATFORM_WIN32

#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h>

#include <rex/platform/process.h>

REX_TEST_CASE("BuildCommandLine round trips through CommandLineToArgvW", "[core][relaunch]") {
  const std::vector<std::string> args{R"(D:\Games\Blue Dragon\)",
                                      "--flag",
                                      R"(say "hi")",
                                      "",
                                      R"(tail\)",
                                      R"(a\"b)",
                                      R"(C:\plain)"};
  const std::wstring command = rex::platform::detail::BuildCommandLine(L"C:\app\app.exe", args);
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(command.c_str(), &argc);
  REQUIRE(argv != nullptr);
  REQUIRE(static_cast<size_t>(argc) == args.size() + 1);
  for (size_t i = 0; i < args.size(); ++i) {
    CHECK(std::wstring(argv[i + 1]) == std::wstring(args[i].begin(), args[i].end()));
  }
  LocalFree(static_cast<void*>(argv));
}

#endif

REX_TEST_CASE("A relaunched process reports its directory", "[.][relaunch_child]") {
  const auto report = rex::platform::GetEnvironmentValue("REX_RELAUNCH_REPORT");
  if (!report) {
    return;
  }
  std::ofstream(*report) << std::filesystem::current_path().string();
}

REX_TEST_CASE("Relaunch starts the given executable in the given directory", "[core][relaunch]") {
  rex::test::TempFolder folder;
  const std::filesystem::path directory = folder.path / "work";
  std::filesystem::create_directories(directory);
  const std::filesystem::path report = folder.path / "report.txt";
  REQUIRE(rex::platform::SetEnvironmentValue("REX_RELAUNCH_REPORT", report.string()));
  const std::vector<std::string> args{"A relaunched process reports its directory"};
  const auto launched =
      rex::platform::Relaunch(rex::platform::GetExecutablePath(), args, directory);
  rex::platform::UnsetEnvironmentValue("REX_RELAUNCH_REPORT");
  REQUIRE(launched);
  std::string reported;
  for (int attempt = 0; attempt < 600 && reported.empty(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ifstream in(report);
    std::getline(in, reported);
  }
  REQUIRE_FALSE(reported.empty());
  CHECK(std::filesystem::equivalent(reported, directory));
}

REX_TEST_CASE("Relaunch of a missing executable or directory fails", "[core][relaunch]") {
  rex::test::TempFolder folder;
  const std::vector<std::string> args;
  const auto missing_exe = rex::platform::Relaunch(folder.path / "missing", args);
  REQUIRE_FALSE(missing_exe);
  CHECK(missing_exe.error().category == rex::ErrorCategory::kPlatform);
  const auto missing_directory =
      rex::platform::Relaunch(rex::platform::GetExecutablePath(), args, folder.path / "missing");
  REQUIRE_FALSE(missing_directory);
  CHECK(missing_directory.error().category == rex::ErrorCategory::kPlatform);
}
