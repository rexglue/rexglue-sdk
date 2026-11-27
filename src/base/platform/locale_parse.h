/**
 * @file        base/platform/locale_parse.h
 * @brief       Parsing of locale tags and the POSIX locale environment
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/locale.h>

namespace rex::platform::detail {

/**
 * Parses one locale tag such as en_US.UTF-8, de_DE@euro or zh-Hant-TW. A codeset after a dot and a
 * modifier after an at sign are dropped, a dash or underscore separates the parts, and the
 * country is the first part of 2 letters after the language.
 *
 * @param tag The locale tag.
 * @return The locale, or nothing for an empty tag, C, POSIX or a tag that starts with no language.
 */
[[nodiscard]] REX_API std::optional<HostLocale> ParseLocaleTag(std::string_view tag);

/**
 * Lists the locales of the POSIX locale environment in the order the user prefers them. The
 * language list comes first with its colon separated tags in order, then the override, the
 * messages category and the default.
 *
 * @param language The value of LANGUAGE.
 * @param lc_all The value of LC_ALL.
 * @param lc_messages The value of LC_MESSAGES.
 * @param lang The value of LANG.
 * @return The locales that parse, in order.
 */
[[nodiscard]] REX_API std::vector<HostLocale> ParseLocaleEnvironment(std::string_view language,
                                                                     std::string_view lc_all,
                                                                     std::string_view lc_messages,
                                                                     std::string_view lang);

}  // namespace rex::platform::detail
