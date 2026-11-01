/**
 * @file        base/relaunch_posix.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <unistd.h>

#include <rex/platform/file.h>

namespace rex::platform {
namespace {

Result<void> Spawn(const std::string& exe, std::span<const std::string> args,
                   const std::string& working_directory) {
  std::vector<std::string> storage;
  storage.push_back(exe);
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) {
    return Err<void>(ErrorCategory::kPlatform,
                     std::format("fork failed ({})", std::strerror(errno)));
  }
  if (pid == 0) {
    setsid();
    if (!working_directory.empty() && chdir(working_directory.c_str()) != 0) {
      _exit(127);
    }
    execv(exe.c_str(), argv.data());
    _exit(127);
  }
  return Ok();
}

}  // namespace

[[nodiscard]] Result<void> Relaunch(std::span<const std::string> args) {
  const auto appimage = GetEnvironmentValue("APPIMAGE");
  return Spawn(appimage && !appimage->empty() ? *appimage : platform::GetExecutablePath().string(),
               args, {});
}

[[nodiscard]] Result<void> Relaunch(const std::filesystem::path& exe,
                                    std::span<const std::string> args,
                                    const std::filesystem::path& working_directory) {
  if (access(exe.c_str(), X_OK) != 0) {
    return Err<void>(ErrorCategory::kPlatform,
                     std::format("{} cannot be started ({})", exe.string(), std::strerror(errno)));
  }
  std::error_code ec;
  if (!working_directory.empty() && !std::filesystem::is_directory(working_directory, ec)) {
    return Err<void>(ErrorCategory::kPlatform,
                     std::format("{} is not a directory", working_directory.string()));
  }
  return Spawn(exe.string(), args, working_directory.string());
}

bool CanRelaunch() {
  const auto desktop = GetEnvironmentValue("XDG_CURRENT_DESKTOP");
  if (desktop && desktop->find("gamescope") != std::string::npos) {
    return false;
  }
  return !GetEnvironmentValue("GAMESCOPE_WAYLAND_DISPLAY").has_value();
}

}  // namespace rex::platform
