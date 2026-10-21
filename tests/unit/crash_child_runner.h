/**
 * @file        tests/unit/crash_child_runner.h
 * @brief       Runs the rex-crash-test child process in a named mode and captures its output
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

  /**
   * Reports whether the child printed a crash report.
   *
   * @return True when the output holds the crash report banner.
   */
  bool WasReported() const { return Has("rex-crash-child crash report"); }

  /**
   * Reports whether the output holds a text.
   *
   * @param text The text to look for.
   * @return True when the output contains the text.
   */
  bool Has(std::string_view text) const { return output.find(text) != std::string::npos; }
};

/**
 * Runs rex-crash-test next to the test executable in a mode and waits for it to end.
 *
 * @param mode The mode name passed to the child.
 * @return The exit status and the output of the child.
 */
ChildResult RunChild(std::string_view mode);

}  // namespace rex::test
