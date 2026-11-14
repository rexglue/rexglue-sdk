/**
 * @file        rexglue/main.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "cli_context.h"
#include "codegen/tool_runtime.h"
#include "log_categories.h"
#include "command/codegen_command.h"
#include "command/init_command.h"
#include "command/vfs_command.h"
#include "command/list_command.h"
#include "command/test_recompiler.h"
#include "ui/ui.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <format>

#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/logging/logger.h>
#include <rex/logging/log_category.h>
#include <rex/platform/process.h>
#include <rex/result.h>
#include <rex/version.h>

namespace {

std::string BannerString() {
  return std::format("ReXGlue v{} Recompilation Toolkit", REXGLUE_VERSION_STRING);
}

bool IsStderrTty() {
  return rex::platform::IsTerminal(stderr);
}

bool ColorEnabled(bool tty) {
  auto nc = rex::platform::GetEnvironmentValue("NO_COLOR");
  if (nc.has_value() && !nc->empty()) {
    return false;
  }
  return tty;
}

std::unique_ptr<rex::log::LogFileSink> g_file_sink;
rex::ScopeGuard g_file_sink_guard;
rex::ScopeGuard g_ui_sink_guard;

[[nodiscard]] rex::Result<void> StartLogging(const std::string& level, const std::string& log_file,
                                             bool verbose) {
  rex::Logger& logger = REX_LOGGER();
  const rex::log::Level parsed_level = verbose ? rex::log::Level::kTrace : [&] {
    auto parsed = rex::log::ParseLevel(level);
    return parsed ? *parsed : rex::log::Level::kInfo;
  }();
  if (!verbose) {
    if (auto parsed = rex::log::ParseLevel(level); !parsed) {
      return std::unexpected(parsed.error());
    }
  }
  REX_TRY_VOID(logger.level.Set(std::string(rex::log::LevelName(parsed_level))));
  REX_TRY_VOID(logger.console.Set(false));
  REX_TRY_VOID(logger.debugger.Set(false));
  REX_TRY_VOID(logger.pattern.Set("%^%Y-%m-%d %H:%M:%S.%e %Q %j %v%$"));
  REX_TRY_VOID(logger.AddCategories(rexglue::kLogCategories));
  g_ui_sink_guard = logger.AddSink(rexglue::ui::GetLogSink());
  if (!log_file.empty()) {
    REX_TRY(g_file_sink, rex::log::LogFileSink::Open(
                             log_file, {rex::log::TimestampStyle::kDateTime, true, true, false}));
    g_file_sink_guard = logger.AddSink(*g_file_sink);
  }
  return rex::Ok();
}

void StopLogging() {
  rex::Logger& logger = REX_LOGGER();
  if (g_file_sink) {
    g_file_sink_guard.Reset();
    g_file_sink.reset();
  }
  g_ui_sink_guard.Reset();
  logger.RemoveCategories(rexglue::kLogCategories);
  logger.Flush();
}

int RunCLI(rex::codegen::ToolApp& tools, const rex::CLIArgList& args) {
  CLI::App app{BannerString(), "rexglue"};
  app.set_version_flag("--version", REXGLUE_VERSION_STRING);
  app.require_subcommand(1);
  app.fallthrough();

  rexglue::cli::CLIContext ctx;
  ctx.tools = &tools;
  std::string log_level = "info";
  std::string log_file;
  bool verbose = false;
  bool force = false;

  app.add_option("--log-level", log_level,
                 "Log level (trace, dev, debug, info, warn, error, critical, off)")
      ->type_name("LEVEL");
  app.add_option("--log-file", log_file, "Append diagnostics to file")->type_name("PATH");
  app.add_flag("-v,--verbose", verbose, "Equivalent to --log-level=trace");
  app.add_flag(
      "-f,--force", force,
      "Replace existing init files, skip the stamp check and write despite validation errors");

  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterCodegen(app, ctx, pending);
  rexglue::cli::RegisterInit(app, ctx, pending);
  rexglue::cli::RegisterRecompileTests(app, ctx, pending);
  rexglue::cli::RegisterVFS(app, ctx, pending);
  rexglue::cli::RegisterList(app, ctx, pending);

  std::vector<std::string> reversed(args.GetAll().rbegin(), args.GetAll().rend());
  try {
    app.parse(std::move(reversed));
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  }

  ctx.verbose = verbose;
  ctx.overwrite_existing = force;
  ctx.generate_despite_errors = force;

  bool tty = IsStderrTty();
  rexglue::ui::Init({.tty = tty, .color = ColorEnabled(tty)});
  if (auto started = StartLogging(log_level, log_file, verbose); !started) {
    rexglue::ui::FailureSummary(started.error().GetDescription(), std::chrono::milliseconds(0));
    rexglue::ui::Shutdown();
    return 1;
  }
  rexglue::ui::Banner(BannerString());

  auto start = std::chrono::steady_clock::now();
  rex::Result<void> result = pending ? pending() : rex::Ok();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  int exit_code = 0;
  if (!result) {
    rexglue::ui::FailureSummary(result.error().GetDescription(), elapsed);
    exit_code = 1;
  } else {
    rexglue::ui::DoneSummary(elapsed);
  }
  StopLogging();
  rexglue::ui::Shutdown();
  return exit_code;
}

class RexglueApp final : public rex::codegen::ToolApp {
 public:
  RexglueApp() : ToolApp("rexglue") {}
};

int RexglueMain(RexglueApp& app, const rex::CLIArgList& args) {
  const int code = RunCLI(app, args);
  app.Shutdown();
  return code;
}

}  // namespace

REX_CONSOLE_APP_MAIN(RexglueApp, RexglueMain)
