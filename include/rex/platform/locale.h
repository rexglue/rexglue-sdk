/**
 * @file        platform/locale.h
 * @brief       Preferred locales of the host
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <rex/platform.h>

namespace rex::platform {

/** A language and country the host prefers. */
struct HostLocale {
  /** Language code such as en or ja. */
  std::string language;
  /** Country code such as US, empty when the system gives none. */
  std::string country;
};

/**
 * Lists the preferred locales of the host user, most preferred first. It reads the operating system
 * and needs no window or UI driver. Language codes are lowercase and country codes uppercase.
 *
 * @return The locales, empty when the system names none.
 */
[[nodiscard]] REX_API std::vector<HostLocale> GetPreferredLocales();

}  // namespace rex::platform
