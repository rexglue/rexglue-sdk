/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/language.h
 * @brief       Mapping of host locales to title languages
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>
#include <format>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <functional>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/stream.h>
#include <rex/kernel/types/content.h>
#include <rex/result.h>
#include <rex/filesystem/xdbf.h>
#include <rex/utility.h>
#include <rex/module.h>
#include <rex/title.h>
#include <rex/ui/window.h>

namespace rex {
namespace ui {
class IUISystem;
}  // namespace ui
}  // namespace rex

namespace rex::xam {

/**
 * Maps a host locale to a title language, ignoring case. Chinese goes by country, so TW, HK and MO
 * give Traditional Chinese and anything else gives Simplified Chinese.
 *
 * @param locale The host locale, language such as zh and country such as TW.
 * @return The title language, or XLanguage::kInvalid when the locale names none.
 */
[[nodiscard]] REX_XAM_API rex::kernel::XLanguage GetLanguageForLocale(const ui::HostLocale& locale);

/**
 * Picks the title language of the first locale of a list that maps to one.
 *
 * @param locales The locales, most preferred first.
 * @return The title language, or XLanguage::kEnglish when no locale maps.
 */
[[nodiscard]] REX_XAM_API rex::kernel::XLanguage GetLanguageForLocales(
    std::span<const ui::HostLocale> locales);

/** Picks the title language of the host. */
REX_DECLARE_INTERFACE(IXAMLanguage, 2)
  virtual ~IXAMLanguage() = default;

  /**
   * Picks the title language from the preferred locales of the host.
   *
   * @return The title language, or XLanguage::kEnglish when no locale maps.
   */
  [[nodiscard]] virtual rex::kernel::XLanguage DetectHostLanguage(rex::ui::IUISystem& ui) = 0;
};

class REX_XAM_API XAMLanguage final : public IXAMLanguage {
 public:
  rex::kernel::XLanguage DetectHostLanguage(rex::ui::IUISystem& ui) override;
};

/**
 * Returns the name of a title language as the XDK documents it for XGetLanguage. The name is such
 * as "Chinese (Traditional)", the XDK placeholder text for kSChineseDeprecated, or "Unknown" for
 * kInvalid and kMaxLanguages.
 */
[[nodiscard]] REX_XAM_API std::string_view GetLanguageName(rex::kernel::XLanguage language);

/**
 * Parses a language code, ignoring case. The codes are en, ja, de, fr, es, it, ko, pt, pl, ru, sv,
 * tr, nb, no and nl with an optional region suffix after a dash or underscore. The code zh gives
 * Simplified Chinese, or Traditional Chinese when a suffix names TW, HK, MO or Hant.
 *
 * @return The title language, or XLanguage::kInvalid for an unknown code, including "auto".
 */
[[nodiscard]] REX_XAM_API rex::kernel::XLanguage ParseLanguageCode(std::string_view code);

}  // namespace rex::xam
