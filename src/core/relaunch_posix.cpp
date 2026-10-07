/**
 * @file        core/relaunch_posix.cpp
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
#include <format>
#include <string>
#include <vector>

#include <unistd.h>

#include <rex/platform/env.h>
#include <rex/platform/file.h>

namespace rex::platform {

[[nodiscard]] Result<void> Relaunch(std::span<const std::string> args) {
  const auto appimage = GetEnvironmentValue("APPIMAGE");
  const std::string exe =
      appimage && !appimage->empty() ? *appimage : platform::GetExecutablePath().string();
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
  const auto desktop = GetEnvironmentValue("XDG_CURRENT_DESKTOP");
  if (desktop && desktop->find("gamescope") != std::string::npos) {
    return false;
  }
  return !GetEnvironmentValue("GAMESCOPE_WAYLAND_DISPLAY").has_value();
}

}  // namespace rex::platform
