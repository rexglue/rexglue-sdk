/**
 * @file        kernel/title_debug_output.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

namespace rex::kernel {

/** What XAM calls to log the debug text a title prints. */
class ITitleDebugOutput {
 public:
  virtual ~ITitleDebugOutput() = default;

  /**
   * Logs debug text the title printed, in the title category.
   */
  virtual void WriteTitleDebugString(std::string_view text) = 0;
};

}  // namespace rex::kernel
