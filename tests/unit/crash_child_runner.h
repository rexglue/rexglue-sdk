/**
 * @file        tests/unit/crash_child_runner.h
 * @brief       Runs rex-crash-test in a named mode and captures its output
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>

namespace rex::test {

/** The exit status and the combined stdout and stderr of a finished child. */
struct ChildResult {
  int status = 0;
  std::string output;

  bool WasReported() const { return Has("rex-crash-child crash report"); }

  bool Has(std::string_view text) const { return output.find(text) != std::string::npos; }
};

/** Runs rex-crash-test in a mode and returns its exit status and output. */
ChildResult RunChild(std::string_view mode);

}  // namespace rex::test
