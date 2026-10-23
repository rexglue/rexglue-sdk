/**
 * @file        runtime/language.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string_view>

#include <rex/export.h>
#include <rex/runtime/xcontent_data.h>

namespace rex::ui {
struct HostLocale;
class IUserInterface;
}  // namespace rex::ui

namespace rex::runtime {

/**
 * Maps a host locale to a title language. Chinese goes by country because the host reports no
 * script, so TW, HK and MO give Traditional Chinese and anything else gives Simplified Chinese.
 * Language and country are compared ignoring case.
 *
 * @param locale The host locale, language such as zh and country such as TW.
 * @return The title language, or XLanguage::kInvalid when the locale names none.
 */
[[nodiscard]] REX_RUNTIME_API XLanguage GetLanguageForLocale(const ui::HostLocale& locale);

/**
 * Picks the title language of the first locale of a list that maps to one.
 *
 * @param locales The locales, most preferred first.
 * @return The title language, or XLanguage::kEnglish when no locale maps.
 */
[[nodiscard]] REX_RUNTIME_API XLanguage
GetLanguageForLocales(std::span<const ui::HostLocale> locales);

/**
 * Picks the title language from the preferred locales of the host.
 *
 * @param ui The host interface.
 * @return The title language, or XLanguage::kEnglish when no locale maps.
 */
[[nodiscard]] REX_RUNTIME_API XLanguage DetectHostLanguage(ui::IUserInterface& ui);

/**
 * Returns the name of a title language as the XDK documents it for XGetLanguage. Host detection
 * never gives kSChineseDeprecated, but a configured value keeps it.
 *
 * @param language The language.
 * @return The name such as "Chinese (Traditional)", the XDK placeholder text for
 *         kSChineseDeprecated, or "Unknown" for kInvalid and kMaxLanguages.
 */
[[nodiscard]] REX_RUNTIME_API std::string_view GetLanguageName(XLanguage language);

/**
 * Parses a language code, ignoring case. The codes are en, ja, de, fr, es, it, ko, pt, pl, ru, sv,
 * tr, nb, no and nl, each with an optional region suffix after a dash or underscore. The code zh
 * gives Simplified Chinese, and Traditional Chinese when a suffix names TW, HK, MO or Hant.
 *
 * @param code The language code.
 * @return The title language, or XLanguage::kInvalid for an unknown code, including "auto".
 */
[[nodiscard]] REX_RUNTIME_API XLanguage ParseLanguageCode(std::string_view code);

}  // namespace rex::runtime
