/**
 * @file        no_throw.h
 * @brief       Teardown guard that logs instead of throwing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <exception>
#include <utility>

#include <rex/logging/category.h>

namespace rex::log {

/**
 * Runs a teardown step and logs anything it throws instead of letting it escape.
 *
 * @tparam Fn Callable that takes no arguments.
 * @param category Category of the log line for a throw.
 * @param fn The step.
 */
template <typename Fn>
void NoThrow(const Category& category, Fn&& fn) noexcept {
  try {
    std::forward<Fn>(fn)();
  } catch (const std::exception& error) {
    detail::WriteFormatted(category, Level::kError, "teardown step failed, {}", error.what());
  } catch (...) {
    detail::WriteFormatted(category, Level::kError, "teardown step failed, unknown exception");
  }
}

}  // namespace rex::log
