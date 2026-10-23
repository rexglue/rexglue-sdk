#include <rex/runtime/language.h>

#include <array>
#include <string>

#include <rex/string/utf8.h>
#include <rex/ui/user_interface.h>

namespace rex::runtime {

namespace {

struct LanguageCode {
  std::string_view code;
  XLanguage language;
};

constexpr auto kLanguageCodes = std::to_array<LanguageCode>({
    {"en", XLanguage::kEnglish},
    {"ja", XLanguage::kJapanese},
    {"de", XLanguage::kGerman},
    {"fr", XLanguage::kFrench},
    {"es", XLanguage::kSpanish},
    {"it", XLanguage::kItalian},
    {"ko", XLanguage::kKorean},
    {"pt", XLanguage::kPortuguese},
    {"pl", XLanguage::kPolish},
    {"ru", XLanguage::kRussian},
    {"sv", XLanguage::kSwedish},
    {"tr", XLanguage::kTurkish},
    {"nb", XLanguage::kNorwegian},
    {"no", XLanguage::kNorwegian},
    {"nl", XLanguage::kDutch},
});

bool IsTraditionalRegion(std::string_view tag) {
  return utf8_equal_case(tag, "TW") || utf8_equal_case(tag, "HK") || utf8_equal_case(tag, "MO");
}

XLanguage FindLanguage(std::string_view base) {
  for (const LanguageCode& entry : kLanguageCodes) {
    if (utf8_equal_case(base, entry.code)) {
      return entry.language;
    }
  }
  return XLanguage::kInvalid;
}

}  // namespace

XLanguage GetLanguageForLocale(const ui::HostLocale& locale) {
  if (utf8_equal_case(locale.language, "zh")) {
    return IsTraditionalRegion(locale.country) ? XLanguage::kTChinese : XLanguage::kSChinese;
  }
  return FindLanguage(locale.language);
}

XLanguage GetLanguageForLocales(std::span<const ui::HostLocale> locales) {
  for (const ui::HostLocale& locale : locales) {
    const XLanguage language = GetLanguageForLocale(locale);
    if (language != XLanguage::kInvalid) {
      return language;
    }
  }
  return XLanguage::kEnglish;
}

XLanguage DetectHostLanguage(ui::IUserInterface& ui) {
  return GetLanguageForLocales(ui.GetPreferredLocales());
}

std::string_view GetLanguageName(XLanguage language) {
  switch (language) {
    case XLanguage::kEnglish:
      return "English";
    case XLanguage::kJapanese:
      return "Japanese";
    case XLanguage::kGerman:
      return "German";
    case XLanguage::kFrench:
      return "French";
    case XLanguage::kSpanish:
      return "Spanish";
    case XLanguage::kItalian:
      return "Italian";
    case XLanguage::kKorean:
      return "Korean";
    case XLanguage::kTChinese:
      return "Chinese (Traditional)";
    case XLanguage::kPortuguese:
      return "Portuguese";
    case XLanguage::kSChineseDeprecated:
      return "Placeholder for deprecated constant for Simplified Chinese";
    case XLanguage::kSChinese:
      return "Chinese (Simplified)";
    case XLanguage::kPolish:
      return "Polish";
    case XLanguage::kRussian:
      return "Russian";
    case XLanguage::kSwedish:
      return "Swedish";
    case XLanguage::kTurkish:
      return "Turkish";
    case XLanguage::kNorwegian:
      return "Norwegian";
    case XLanguage::kDutch:
      return "Dutch";
    default:
      return "Unknown";
  }
}

XLanguage ParseLanguageCode(std::string_view code) {
  const size_t split = code.find_first_of("-_");
  const std::string_view base = code.substr(0, split);
  if (!utf8_equal_case(base, "zh")) {
    return FindLanguage(base);
  }
  std::string_view rest = split == std::string_view::npos ? std::string_view{} : code.substr(split);
  while (!rest.empty()) {
    rest.remove_prefix(1);
    const size_t next = rest.find_first_of("-_");
    const std::string_view tag = rest.substr(0, next);
    if (IsTraditionalRegion(tag) || utf8_equal_case(tag, "Hant")) {
      return XLanguage::kTChinese;
    }
    rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);
  }
  return XLanguage::kSChinese;
}

}  // namespace rex::runtime
