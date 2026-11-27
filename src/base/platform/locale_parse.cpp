#include "base/platform/locale_parse.h"

#include <cctype>
#include <string>

namespace rex::platform::detail {

namespace {

bool IsLetters(std::string_view text, size_t min, size_t max) {
  if (text.size() < min || text.size() > max) {
    return false;
  }
  for (const char c : text) {
    if (!std::isalpha(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string Upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

std::optional<HostLocale> ParseLocaleTag(std::string_view tag) {
  tag = tag.substr(0, tag.find_first_of(".@"));
  size_t split = tag.find_first_of("-_");
  const std::string_view language = tag.substr(0, split);
  if (!IsLetters(language, 2, 3)) {
    return std::nullopt;
  }
  HostLocale locale{.language = Lower(language), .country = {}};
  while (split != std::string_view::npos) {
    tag.remove_prefix(split + 1);
    split = tag.find_first_of("-_");
    const std::string_view part = tag.substr(0, split);
    if (IsLetters(part, 2, 2)) {
      locale.country = Upper(part);
      break;
    }
  }
  return locale;
}

std::vector<HostLocale> ParseLocaleEnvironment(std::string_view language, std::string_view lc_all,
                                               std::string_view lc_messages,
                                               std::string_view lang) {
  std::vector<HostLocale> locales;
  while (!language.empty()) {
    const size_t end = language.find(':');
    if (auto locale = ParseLocaleTag(language.substr(0, end))) {
      locales.push_back(std::move(*locale));
    }
    language = end == std::string_view::npos ? std::string_view{} : language.substr(end + 1);
  }
  for (const std::string_view tag : {lc_all, lc_messages, lang}) {
    if (auto locale = ParseLocaleTag(tag)) {
      locales.push_back(std::move(*locale));
    }
  }
  return locales;
}

}  // namespace rex::platform::detail
