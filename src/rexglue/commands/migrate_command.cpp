/**
 * @file        rexglue/commands/migrate_command.cpp
 * @brief       Project migration command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include "migrate_command.h"
#include "template_utils.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <rex/codegen/config.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/version.h>

namespace fs = std::filesystem;

namespace rexglue::cli {

using rex::Err;
using rex::ErrorCategory;
using rex::Ok;
using rex::Result;
namespace {

// New-style slim main.cpp
std::string generate_main_cpp(const AppNameParts& names) {
  std::string class_name = names.pascal_case + "App";
  std::string content;

  content += "// " + names.snake_case + " - ReXGlue Recompiled Project\n";
  content += "//\n";
  content += "// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.\n";
  content += "\n";
  content += "#include \"generated/" + names.snake_case + "_config.h\"\n";
  content += "#include \"generated/" + names.snake_case + "_init.h\"\n";
  content += "\n";
  content += "#include \"" + names.snake_case + "_app.h\"\n";
  content += "\n";
  content += "REX_DEFINE_APP(" + names.snake_case + ", " + class_name + "::Create)\n";

  return content;
}

// New-style slim CMakeLists.txt
std::string generate_cmakelists(const AppNameParts& names) {
  std::string content;

  content += "# " + names.snake_case + " - ReXGlue Recompiled Project\n";
  content += "#\n";
  content += "# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.\n";
  content += "\n";
  content += "cmake_minimum_required(VERSION 3.25)\n";
  content += "project(" + names.snake_case + " LANGUAGES CXX)\n";
  content += "\n";
  content += "set(CMAKE_CXX_STANDARD 23)\n";
  content += "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
  content += "\n";
  content += "include(generated/rexglue.cmake)\n";
  content += "\n";
  content += "# Sources\n";
  content += "set(" + names.upper_case + "_SOURCES\n";
  content += "    src/main.cpp\n";
  content += ")\n";
  content += "\n";
  content += "if(WIN32)\n";
  content +=
      "    add_executable(" + names.snake_case + " WIN32 ${" + names.upper_case + "_SOURCES})\n";
  content += "else()\n";
  content += "    add_executable(" + names.snake_case + " ${" + names.upper_case + "_SOURCES})\n";
  content += "endif()\n";
  content += "\n";
  content += "rexglue_setup_target(" + names.snake_case + ")\n";
  content += "\n";

  return content;
}

std::string read_file_content(const fs::path& path) {
  std::ifstream file(path);
  if (!file)
    return {};
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

bool backup_file(const fs::path& path) {
  if (!fs::exists(path))
    return true;
  fs::path backup = path;
  backup += ".bak";
  std::error_code ec;
  fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    REXLOG_ERROR("Failed to back up {}: {}", path.string(), ec.message());
    return false;
  }
  REXLOG_INFO("  Backed up {} -> {}", path.filename().string(), backup.filename().string());
  return true;
}

bool is_old_style_project(const fs::path& root) {
  return !fs::exists(root / "generated" / "rexglue.cmake");
}

}  // anonymous namespace

Result<void> MigrateProject(const MigrateOptions& opts, const CliContext& ctx) {
  (void)ctx;

  if (opts.app_root.empty()) {
    return Err<void>(ErrorCategory::Config, "--app_root is required");
  }

  fs::path root = fs::absolute(opts.app_root);

  // Validate that this looks like an existing rexglue project
  if (!fs::exists(root)) {
    return Err<void>(ErrorCategory::IO, "Project directory does not exist: " + root.string());
  }
  if (!fs::exists(root / "src" / "main.cpp")) {
    return Err<void>(ErrorCategory::IO,
                     "Not a rexglue project (no src/main.cpp): " + root.string());
  }

  // Find the *_config.toml to read the project name
  fs::path config_path;
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".toml" &&
        entry.path().stem().string().ends_with("_config")) {
      config_path = entry.path();
      break;
    }
  }
  if (config_path.empty()) {
    return Err<void>(ErrorCategory::IO, "No *_config.toml found in project root: " + root.string());
  }

  // Read project name from config
  rex::codegen::RecompilerConfig config;
  config.Load(config_path.string());
  if (config.projectName.empty() || config.projectName == "rex") {
    return Err<void>(ErrorCategory::Config,
                     "Could not read project_name from " + config_path.filename().string());
  }

  auto names = parse_app_name(config.projectName);

  REXLOG_INFO("Migrating project '{}' at: {}", names.snake_case, root.string());

  // Ensure generated/ directory exists
  fs::create_directories(root / "generated");

  // Check project style BEFORE writing anything
  bool old_style = is_old_style_project(root);

  // --- SDK-managed file: always regenerate ---
  std::string new_rexglue_cmake = generate_rexglue_cmake(names, REXGLUE_VERSION_STRING);
  std::string old_rexglue_cmake = read_file_content(root / "generated" / "rexglue.cmake");

  if (old_rexglue_cmake != new_rexglue_cmake) {
    if (!write_file(root / "generated" / "rexglue.cmake", new_rexglue_cmake)) {
      return Err<void>(ErrorCategory::IO, "Failed to write generated/rexglue.cmake");
    }
    REXLOG_INFO("  Updated generated/rexglue.cmake");
  } else {
    REXLOG_INFO("  generated/rexglue.cmake is already up to date");
  }

  // --- User-owned files: create only if missing (or --force) ---
  std::string app_header_name = names.snake_case + "_app.h";
  fs::path app_header_path = root / "src" / app_header_name;
  fs::path main_cpp_path = root / "src" / "main.cpp";
  fs::path cmakelists_path = root / "CMakeLists.txt";

  bool need_user_files = old_style || !fs::exists(app_header_path);

  if (need_user_files && !opts.force) {
    // Migrating from old style - warn about backups
    std::cerr << "This project uses the old file layout and needs migration.\n";
    std::cerr << "The following files will be backed up and replaced:\n";
    std::cerr << "  src/main.cpp -> src/main.cpp.bak\n";
    std::cerr << "  CMakeLists.txt -> CMakeLists.txt.bak\n";
    std::cerr << "New files will be created:\n";
    std::cerr << "  src/" << app_header_name << " (your app class - edit this!)\n";
    std::cerr << "\nContinue? [y/N] " << std::flush;

    std::string answer;
    std::getline(std::cin, answer);
    if (answer.empty() || (answer[0] != 'y' && answer[0] != 'Y')) {
      REXLOG_INFO("Migration cancelled.");
      return Ok();
    }
  }

  if (need_user_files || opts.force) {
    // Back up existing files
    if (!backup_file(main_cpp_path))
      return Err<void>(ErrorCategory::IO, "Failed to back up src/main.cpp");
    if (!backup_file(cmakelists_path))
      return Err<void>(ErrorCategory::IO, "Failed to back up CMakeLists.txt");

    // Write new slim main.cpp
    if (!write_file(main_cpp_path, generate_main_cpp(names))) {
      return Err<void>(ErrorCategory::IO, "Failed to write src/main.cpp");
    }
    REXLOG_INFO("  Wrote src/main.cpp");

    // Write new slim CMakeLists.txt
    if (!write_file(cmakelists_path, generate_cmakelists(names))) {
      return Err<void>(ErrorCategory::IO, "Failed to write CMakeLists.txt");
    }
    REXLOG_INFO("  Wrote CMakeLists.txt");

    // Write app header (only if missing, even with --force we don't want to nuke customizations
    // unless the file doesn't exist yet)
    if (!fs::exists(app_header_path) || opts.force) {
      if (fs::exists(app_header_path)) {
        if (!backup_file(app_header_path))
          return Err<void>(ErrorCategory::IO, "Failed to back up src/" + app_header_name);
      }
      if (!write_file(app_header_path, generate_app_header(names))) {
        return Err<void>(ErrorCategory::IO, "Failed to write src/" + app_header_name);
      }
      REXLOG_INFO("  Wrote src/{}", app_header_name);
    }
  } else {
    REXLOG_INFO("  User files already exist (CMakeLists.txt, src/main.cpp, src/{}). Skipping.",
                app_header_name);
  }

  REXLOG_INFO("Migration complete. Re-run CMake configure to pick up changes.");

  return Ok();
}

}  // namespace rexglue::cli
