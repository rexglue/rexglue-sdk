/**
 * @file        tests/unit/codegen/tool_runtime_test.cpp
 * @brief       The codegen tool runtime under the rexglue tool's logger
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>
#include <string>

#include <rex/logging.h>

#include "codegen/tool_runtime.h"
#include "rexglue/log_categories.h"
#include "test_memory.h"

REX_TEST_CASE("The tool runtime opens under the log categories the rexglue tool registers",
              "[codegen][tool]") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "rex_tool_runtime_log_test";
  std::filesystem::create_directories(root);
  rex::testing::ReleaseTestMemory();
  rex::LogConfig config;
  config.debugger = false;
  config.categories.assign(rexglue::kLogCategories.begin(), rexglue::kLogCategories.end());
  REQUIRE(rex::log::Start(config).has_value());
  {
    auto tool = rex::codegen::OpenToolRuntime(root);
    INFO((tool ? std::string() : tool.error().message));
    CHECK(tool.has_value());
  }
  rex::log::Stop();
  std::filesystem::remove_all(root);
}
