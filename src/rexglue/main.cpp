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
#include "commands/migrate_command.h"
#include "commands/test_recompiler.h"

#include <chrono>
#include <iostream>
#include <map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/version.h>

// Codegen flags (definitions in codegen_flags.cpp)
REXCVAR_DECLARE(bool, force);
REXCVAR_DECLARE(bool, enable_exception_handlers);

// Recompile-tests flags
REXCVAR_DEFINE_STRING(bin_dir, "", "RecompileTests",
                      "Directory containing linked .bin and .map files");
REXCVAR_DEFINE_STRING(asm_dir, "", "RecompileTests",
                      "Directory containing .s assembly source files");
REXCVAR_DEFINE_STRING(output, "", "RecompileTests", "Output path for recompile-tests");

// Init flags
REXCVAR_DEFINE_STRING(app_name, "", "Init", "Project name for init command");
REXCVAR_DEFINE_STRING(app_root, "", "Init", "Project root directory for init command");
REXCVAR_DEFINE_STRING(app_desc, "", "Init", "Project description (optional)");
REXCVAR_DEFINE_STRING(app_author, "", "Init", "Project author (optional)");
REXCVAR_DEFINE_BOOL(sdk_example, false, "Init", "Create as SDK example (omit vcpkg.json)");
REXCVAR_DEFINE_STRING(template_dir, "", "Init", "Custom template directory for overrides");

using rex::Ok;
using rex::Result;

std::string GetTitleString() {
  return fmt::format("ReXGlue v{} - Xbox 360 Recompilation Toolkit", REXGLUE_VERSION_STRING);
}

void PrintUsage() {
  std::cerr << GetTitleString() + "\n\n";
  std::cerr << "Usage: rexglue <command> [flags] [args]\n\n";
  std::cerr << "Commands:\n";
  std::cerr << "  codegen <config.toml>   Analyze XEX and generate C++ code\n";
  std::cerr << "  init                    Initialize a new project\n";
  std::cerr << "  migrate                 Migrate project to current SDK version\n";
  std::cerr << "  recompile-tests         Generate Catch2 tests from PPC assembly\n\n";
  std::cerr << "Run 'rexglue --help' for flag details.\n";
}

int main(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  std::string command;

  if (!remaining.empty()) {
    command = remaining[0];
  }

  if (command.empty()) {
    PrintUsage();
    return 1;
  }

  // Set up logging from CVARs
  std::string level_str = REXCVAR_GET(log_level);
  std::string log_file_path = REXCVAR_GET(log_file);
  bool verbose = REXCVAR_GET(log_verbose);

  // Verbose overrides level if not explicitly set
  if (verbose && level_str == "info") {
    level_str = "trace";
    rex::cvar::SetFlagByName("log_level", "trace");
  }

  std::map<std::string, std::string> category_levels;
  auto log_config = rex::BuildLogConfig(log_file_path.empty() ? nullptr : log_file_path.c_str(),
                                        level_str, category_levels);
  log_config.log_to_console = true;  // CLI always logs to console
  rex::InitLogging(log_config);

  // Register callback for runtime level changes
  rex::RegisterLogLevelCallback();

  REXLOG_INFO(GetTitleString());

  // Set up CLI context
  rexglue::cli::CliContext ctx;
  ctx.verbose = verbose;
  ctx.force = REXCVAR_GET(force);
  ctx.enableExceptionHandlers = REXCVAR_GET(enable_exception_handlers);

  auto startTime = std::chrono::steady_clock::now();

  Result<void> result = Ok();
  if (command == "init") {
    rexglue::cli::InitOptions opts;
    opts.app_name = REXCVAR_GET(app_name);
    opts.app_root = REXCVAR_GET(app_root);
    opts.app_desc = REXCVAR_GET(app_desc);
    opts.app_author = REXCVAR_GET(app_author);
    opts.sdk_example = REXCVAR_GET(sdk_example);
    opts.template_dir = REXCVAR_GET(template_dir);
    opts.force = ctx.force;

    if (opts.app_name.empty()) {
      REXLOG_ERROR("--app_name is required for init command");
      return 1;
    }
    if (opts.app_root.empty()) {
      REXLOG_ERROR("--app_root is required for init command");
      return 1;
    }

    result = rexglue::cli::InitProject(opts, ctx);
  } else if (command == "codegen") {
    if (remaining.size() < 2) {
      REXLOG_ERROR("Missing config path. Usage: rexglue codegen <config.toml>");
      return 1;
    }
    if (remaining.size() > 2) {
      REXLOG_ERROR("Too many arguments for codegen command");
      return 1;
    }
    std::string config_path = remaining[1];
    result = rexglue::cli::CodegenFromConfig(config_path, ctx);
  } else if (command == "recompile-tests") {
    std::string bin_dir = REXCVAR_GET(bin_dir);
    std::string asm_dir = REXCVAR_GET(asm_dir);
    std::string output = REXCVAR_GET(output);

    if (bin_dir.empty() || asm_dir.empty() || output.empty()) {
      REXLOG_ERROR("--bin-dir, --asm-dir, and --output are required");
      return 1;
    }

    if (!rexglue::commands::recompile_tests(bin_dir, asm_dir, output)) {
      REXLOG_ERROR("Test recompilation failed");
      return 1;
    }
  } else if (command == "migrate") {
    rexglue::cli::MigrateOptions opts;
    opts.app_root = REXCVAR_GET(app_root);
    opts.template_dir = REXCVAR_GET(template_dir);
    opts.force = ctx.force;

    if (opts.app_root.empty()) {
      REXLOG_ERROR("--app_root is required for migrate command");
      return 1;
    }

    result = rexglue::cli::MigrateProject(opts, ctx);
  } else {
    REXLOG_ERROR("Unknown command: {}", command);
    PrintUsage();
    return 1;
  }

  auto endTime = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

  if (!result) {
    REXLOG_ERROR("Operation failed: {} (took {:.3f}s)", result.error().what(),
                 elapsed.count() / 1000.0);
    return 1;
  }

  REXLOG_INFO("Operation completed successfully in {:.3f}s", elapsed.count() / 1000.0);
  return 0;
}
