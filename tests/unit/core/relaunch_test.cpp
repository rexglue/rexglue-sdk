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

#include <rex/platform.h>

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
