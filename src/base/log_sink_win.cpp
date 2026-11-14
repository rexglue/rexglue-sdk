/**
 * @file        base/log_sink_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>

#include "base/logger.h"
#include "base/platform_win.h"

namespace rex::log {

std::tm ToLocalTime(std::time_t time) {
  std::tm local{};
  localtime_s(&local, &time);
  return local;
}

int OpenCrashFile(const std::filesystem::path&) {
  return -1;
}

void CloseCrashFile(int) {}

}  // namespace rex::log
