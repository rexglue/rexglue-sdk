/**
 * @file        rexglue/main.cpp
 * @brief       ReXGlue CLI tool entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "cli_utils.h"
#include "commands/codegen_command.h"
#include "commands/init_command.h"
#include "commands/vfs_command.h"
#include "commands/test_recompiler.h"
#include "ui/ui.h"

#include <chrono>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform/console.h>
#include <rex/platform/env.h>
#include <rex/result.h>
#include <rex/version.h>

namespace {

std::string TitleString() {
  return fmt::format("ReXGlue v{} - Xbox 360 Recompilation Toolkit", REXGLUE_VERSION_STRING);
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

rex::Result<void> StartLogging(const std::string& level, const std::string& log_file,
                               bool verbose) {
  rex::LogConfig config;
  config.debugger = false;
  config.format.category = false;
  if (verbose) {
    config.level = rex::log::Level::kTrace;
  } else if (auto parsed = rex::log::ParseLevel(level)) {
    config.level = *parsed;
  } else {
    return std::unexpected(parsed.error());
  }
  config.file.enabled = !log_file.empty();
  config.file.path = log_file;
  config.categories = {&rex::log::codegen, &rex::log::rexglue, &rex::log::system};
  config.sinks = {&rexglue::ui::LogSink()};
  return rex::log::Start(config);
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{TitleString(), "rexglue"};
  app.set_version_flag("--version", REXGLUE_VERSION_STRING);
  app.require_subcommand(1);
  app.fallthrough();

  rexglue::cli::CLIContext ctx;
  std::string log_level = "info";
  std::string log_file;
  bool verbose = false;
  bool force = false;

  app.add_option("--log-level", log_level, "Log level (trace, dev, debug, info, warn, error)")
      ->type_name("LEVEL");
  app.add_option("--log-file", log_file, "Append diagnostics to file")->type_name("PATH");
  app.add_flag("-v,--verbose", verbose, "Equivalent to --log-level=trace");
  app.add_flag("-f,--force", force, "Skip confirmations and proceed");

  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterCodegen(app, ctx, pending);
  rexglue::cli::RegisterInit(app, ctx, pending);
  rexglue::cli::RegisterRecompileTests(app, ctx, pending);
  rexglue::cli::RegisterVFS(app, ctx, pending);

  CLI11_PARSE(app, argc, argv);

  ctx.verbose = verbose;
  ctx.overwrite_existing = force;
  ctx.generate_despite_errors = force;

  bool tty = IsStderrTty();
  rexglue::ui::Init({.tty = tty, .color = ColorEnabled(tty)});
  if (auto started = StartLogging(log_level, log_file, verbose); !started) {
    rexglue::ui::FailureSummary(started.error().what(), std::chrono::milliseconds(0));
    rexglue::ui::Shutdown();
    return 1;
  }
  rexglue::ui::Banner(TitleString());

  auto start = std::chrono::steady_clock::now();
  rex::Result<void> result = pending ? pending() : rex::Ok();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  int exit_code = 0;
  if (!result) {
    rexglue::ui::FailureSummary(result.error().what(), elapsed);
    exit_code = result.error().category == rex::ErrorCategory::UserAbort ? 2 : 1;
  } else {
    rexglue::ui::DoneSummary(elapsed);
  }
  rex::log::Stop();
  rexglue::ui::Shutdown();
  return exit_code;
}
