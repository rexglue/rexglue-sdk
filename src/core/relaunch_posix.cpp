/**
 * @file        core/relaunch_posix.cpp
 * @brief       Relaunch of the running executable on Linux and macOS
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include <unistd.h>

#include <rex/platform/file.h>

namespace rex::platform {

Result<void> Relaunch(std::span<const std::string> args) {
  const char* appimage = std::getenv("APPIMAGE");
  const std::string exe =
      appimage && *appimage ? std::string(appimage) : platform::GetExecutablePath().string();
  std::vector<std::string> storage;
  storage.push_back(exe);
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) {
    return Err<void>(ErrorCategory::Platform,
                     std::format("fork failed ({})", std::strerror(errno)));
  }
  if (pid == 0) {
    setsid();
    execv(exe.c_str(), argv.data());
    _exit(127);
  }
  return Ok();
}

bool CanRelaunch() {
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  if (desktop && std::string_view(desktop).find("gamescope") != std::string_view::npos) {
    return false;
  }
  return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") == nullptr;
}

}  // namespace rex::platform
