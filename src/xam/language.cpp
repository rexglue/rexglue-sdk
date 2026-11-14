#include <rex/xam/language.h>

#include <array>
#include <string>

#include <rex/string/text.h>
#include <rex/ui/ui_system.h>
#include <rex/kernel/types/content.h>

namespace rex::xam {

namespace {

struct LanguageCode {
  std::string_view code;
  rex::kernel::XLanguage language;
};

constexpr auto kLanguageCodes = std::to_array<LanguageCode>({
    {"en", rex::kernel::XLanguage::kEnglish},
    {"ja", rex::kernel::XLanguage::kJapanese},
    {"de", rex::kernel::XLanguage::kGerman},
    {"fr", rex::kernel::XLanguage::kFrench},
    {"es", rex::kernel::XLanguage::kSpanish},
    {"it", rex::kernel::XLanguage::kItalian},
    {"ko", rex::kernel::XLanguage::kKorean},
    {"pt", rex::kernel::XLanguage::kPortuguese},
    {"pl", rex::kernel::XLanguage::kPolish},
    {"ru", rex::kernel::XLanguage::kRussian},
    {"sv", rex::kernel::XLanguage::kSwedish},
    {"tr", rex::kernel::XLanguage::kTurkish},
    {"nb", rex::kernel::XLanguage::kNorwegian},
    {"no", rex::kernel::XLanguage::kNorwegian},
    {"nl", rex::kernel::XLanguage::kDutch},
});

bool IsTraditionalRegion(std::string_view tag) {
  return string::utf8_equal_case(tag, "TW") || string::utf8_equal_case(tag, "HK") ||
         string::utf8_equal_case(tag, "MO");
}

rex::kernel::XLanguage FindLanguage(std::string_view base) {
  for (const LanguageCode& entry : kLanguageCodes) {
    if (string::utf8_equal_case(base, entry.code)) {
      return entry.language;
    }
  }
  return rex::kernel::XLanguage::kInvalid;
}

}  // namespace

rex::kernel::XLanguage GetLanguageForLocale(const ui::HostLocale& locale) {
  if (string::utf8_equal_case(locale.language, "zh")) {
    return IsTraditionalRegion(locale.country) ? rex::kernel::XLanguage::kTChinese
                                               : rex::kernel::XLanguage::kSChinese;
  }
  return FindLanguage(locale.language);
}

rex::kernel::XLanguage GetLanguageForLocales(std::span<const ui::HostLocale> locales) {
  for (const ui::HostLocale& locale : locales) {
    const rex::kernel::XLanguage language = GetLanguageForLocale(locale);
    if (language != rex::kernel::XLanguage::kInvalid) {
      return language;
    }
  }
  return rex::kernel::XLanguage::kEnglish;
}

rex::kernel::XLanguage XAMLanguage::DetectHostLanguage(rex::ui::IUISystem& ui) {
  return GetLanguageForLocales(ui.GetPreferredLocales());
}

std::string_view GetLanguageName(rex::kernel::XLanguage language) {
  switch (language) {
    case rex::kernel::XLanguage::kEnglish:
      return "English";
    case rex::kernel::XLanguage::kJapanese:
      return "Japanese";
    case rex::kernel::XLanguage::kGerman:
      return "German";
    case rex::kernel::XLanguage::kFrench:
      return "French";
    case rex::kernel::XLanguage::kSpanish:
      return "Spanish";
    case rex::kernel::XLanguage::kItalian:
      return "Italian";
    case rex::kernel::XLanguage::kKorean:
      return "Korean";
    case rex::kernel::XLanguage::kTChinese:
      return "Chinese (Traditional)";
    case rex::kernel::XLanguage::kPortuguese:
      return "Portuguese";
    case rex::kernel::XLanguage::kSChineseDeprecated:
      return "Placeholder for deprecated constant for Simplified Chinese";
    case rex::kernel::XLanguage::kSChinese:
      return "Chinese (Simplified)";
    case rex::kernel::XLanguage::kPolish:
      return "Polish";
    case rex::kernel::XLanguage::kRussian:
      return "Russian";
    case rex::kernel::XLanguage::kSwedish:
      return "Swedish";
    case rex::kernel::XLanguage::kTurkish:
      return "Turkish";
    case rex::kernel::XLanguage::kNorwegian:
      return "Norwegian";
    case rex::kernel::XLanguage::kDutch:
      return "Dutch";
    default:
      return "Unknown";
  }
}

rex::kernel::XLanguage ParseLanguageCode(std::string_view code) {
  const size_t split = code.find_first_of("-_");
  const std::string_view base = code.substr(0, split);
  if (!string::utf8_equal_case(base, "zh")) {
    return FindLanguage(base);
  }
  std::string_view rest = split == std::string_view::npos ? std::string_view{} : code.substr(split);
  while (!rest.empty()) {
    rest.remove_prefix(1);
    const size_t next = rest.find_first_of("-_");
    const std::string_view tag = rest.substr(0, next);
    if (IsTraditionalRegion(tag) || string::utf8_equal_case(tag, "Hant")) {
      return rex::kernel::XLanguage::kTChinese;
    }
    rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);
  }
  return rex::kernel::XLanguage::kSChinese;
}

}  // namespace rex::xam
