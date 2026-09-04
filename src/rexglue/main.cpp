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
#include "commands/test_recompiler.h"
#include "ui/ui.h"

#include <chrono>
#include <map>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/cvar_cli.h>
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
  return rex::platform::console::is_tty(stderr);
}

bool ColorEnabled(bool tty) {
  auto nc = rex::platform::env::get("NO_COLOR");
  if (nc.has_value() && !nc->empty()) {
    return false;
  }
  return tty;
}

void ConfigureLogging(const std::string& level, const std::string& log_file, bool verbose) {
  std::map<std::string, std::string> category_levels;
  auto config = rex::BuildLogConfig(log_file.empty() ? nullptr : log_file.c_str(),
                                    verbose ? "trace" : level, category_levels);
  config.log_to_console = true;
  rex::InitLogging(config);
  rex::RegisterLogLevelCallback();
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{TitleString(), "rexglue"};
  app.set_version_flag("--version", REXGLUE_VERSION_STRING);
  app.require_subcommand(1);
  // Lets cvar overrides be spelled after the subcommand name.
  app.fallthrough();

  rexglue::cli::CliContext ctx;
  std::string log_level = "info";
  std::string log_file;
  bool verbose = false;
  bool force = false;

  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error)")
      ->type_name("LEVEL");
  app.add_option("--log-file", log_file, "Append diagnostics to file")->type_name("PATH");
  app.add_flag("-v,--verbose", verbose, "Equivalent to --log-level=trace");
  app.add_flag("-f,--force", force, "Skip confirmations and proceed");

  rex::InitLoggingEarly();
  rex::cvar::ApplyEnvironment();

  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterCodegen(app, ctx, pending);
  rexglue::cli::RegisterInit(app, ctx, pending);
  rexglue::cli::RegisterRecompileTests(app, ctx, pending);

  rex::cvar::RegisterCliOptions(app);

  CLI11_PARSE(app, argc, argv);

  // The registry also exposes log_level/log_file, so honor those spellings.
  if (rex::cvar::GetFlagSource("log_level") == rex::cvar::Source::kCommandLine) {
    log_level = rex::cvar::GetFlagByName("log_level");
  }
  if (rex::cvar::GetFlagSource("log_file") == rex::cvar::Source::kCommandLine) {
    log_file = rex::cvar::GetFlagByName("log_file");
  }

  ConfigureLogging(log_level, log_file, verbose);
  ctx.verbose = verbose;
  ctx.overwrite_existing = force;
  ctx.generate_despite_errors = force;

  bool tty = IsStderrTty();
  rexglue::ui::Init({.tty = tty, .color = ColorEnabled(tty)});
  rexglue::ui::Banner(TitleString());

  auto start = std::chrono::steady_clock::now();
  // A command reports failure through Result, but anything it THROWS escaped
  // this frame: no handler, no summary, the process dies as 0xE06D7363 and the
  // message the thrower took care to build is never seen. FunctionGraph::sealAll
  // is one such -- "N functions cannot be sealed: <which, and why>" -- and it
  // took a symbolised crash dump to learn that from a Forza Horizon codegen that
  // just vanished after the Write banner. Report it like any other failure.
  rex::Result<void> result = rex::Ok();
  try {
    if (pending) {
      result = pending();
    }
  } catch (const std::exception& e) {
    result = rex::Err(rex::ErrorCategory::Runtime, e.what());
  } catch (...) {
    result = rex::Err(rex::ErrorCategory::Runtime, "unhandled non-std exception");
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  int exit_code = 0;
  if (!result) {
    rexglue::ui::FailureSummary(result.error().what(), elapsed);
    exit_code = result.error().category == rex::ErrorCategory::UserAbort ? 2 : 1;
  } else {
    rexglue::ui::DoneSummary(elapsed);
  }
  rexglue::ui::Shutdown();
  return exit_code;
}
