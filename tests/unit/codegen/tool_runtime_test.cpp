/**
 * @file        tests/unit/codegen/tool_runtime_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"
#include "temp_folder.h"

#include <filesystem>
#include <string>

#include <rex/app/app.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/kernel/title_runtime.h>

#include "codegen/tool_runtime.h"
#include "rexglue/log_categories.h"
#include "kernel/title_state.h"
#include "test_memory.h"

REX_TEST_CASE("The tool runtime opens under the log categories the rexglue tool registers",
              "[codegen][tool]") {
  const rex::test::TempFolder folder("rex_tool_runtime_log_test_");
  const rex::test::TempFolder app_root("rex_tool_runtime_log_app_");
  const std::filesystem::path& root = folder.path;
  rex::test::ReleaseTestMemory();
  rex::Logger& logger = REX_LOGGER();
  REQUIRE(logger.debugger.Set(false));
  REQUIRE(logger.AddCategories(rexglue::kLogCategories));
  {
    rex::test::ObjectApp<rex::codegen::ToolApp> app("tool_runtime_test", app_root.path);
    auto tool = app.Open(root);
    INFO((tool ? std::string() : tool.error().message));
    CHECK(tool.has_value());
  }
  logger.RemoveCategories(rexglue::kLogCategories);
}

namespace {

class CrashProbe final : public rex::test::ObjectApp<rex::codegen::ToolApp> {
 public:
  CrashProbe() : ObjectApp("crash_probe") {}
  using ToolApp::GetCrashOptions;
};

class PlainApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  PlainApp() : ObjectApp("plain_app") {}
  using App::GetCrashOptions;
};

}  // namespace

REX_TEST_CASE("console app crash options show no dialog", "[codegen][tool]") {
  CHECK_FALSE(CrashProbe().GetCrashOptions().show_dialog);
  CHECK_FALSE(PlainApp().GetCrashOptions().show_dialog);
}

REX_TEST_CASE("codegen loads without launching", "[codegen][tool]") {
  const rex::test::TempFolder folder("rex_tool_runtime_load_test_");
  const rex::test::TempFolder app_root("rex_tool_runtime_load_app_");
  rex::test::ReleaseTestMemory();
  rex::test::ObjectApp<rex::codegen::ToolApp> app("tool_runtime_test", app_root.path);
  auto tool = app.Open(folder.path);
  REQUIRE(tool.has_value());
  rex::kernel::TitleRuntime& runtime = tool->GetRuntime();
  CHECK(runtime.IsLoaded());
  CHECK_FALSE(runtime.IsRunning());
  CHECK(StateOf(runtime).GetFileSystem() != nullptr);
}
