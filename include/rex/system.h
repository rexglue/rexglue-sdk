/**
 * @file        system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/system.h (Ben Vanik, 2020).
 */

#pragma once

#include <string_view>

#include <rex/macros.h>
#include <rex/platform.h>

namespace rex {

/**
 * Icon and severity of a simple message box.
 */
enum class SimpleMessageBoxType {
  Help,    /**< Informational message. */
  Warning, /**< Warning message. */
  Error,   /**< Error message. */
};

/**
 * Shows a native message box and blocks the caller until it closes. On POSIX it writes the message
 * to stderr instead.
 *
 * @param type The icon and severity.
 * @param message The UTF-8 message text.
 */
REX_API void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message);

}  // namespace rex
