/**
 * @file        tests/unit/base/locale_parse_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/platform/locale_parse.h"

using rex::platform::HostLocale;
using rex::platform::detail::ParseLocaleEnvironment;
using rex::platform::detail::ParseLocaleTag;

namespace {

std::string Join(const std::vector<HostLocale>& locales) {
  std::string out;
  for (const HostLocale& locale : locales) {
    out += locale.language + "/" + locale.country + " ";
  }
  return out;
}

std::string JoinTag(std::string_view tag) {
  const auto locale = ParseLocaleTag(tag);
  return locale ? Join({*locale}) : std::string("none");
}

}  // namespace

REX_TEST_CASE("a locale tag gives lowercase language and uppercase country", "[base][locale]") {
  CHECK(JoinTag("en_US.UTF-8") == "en/US ");
  CHECK(JoinTag("de_DE@euro") == "de/DE ");
  CHECK(JoinTag("JA_jp") == "ja/JP ");
  CHECK(JoinTag("fr") == "fr/ ");
  CHECK(JoinTag("zh-Hant-TW") == "zh/TW ");
  CHECK(JoinTag("pt-BR") == "pt/BR ");
}

REX_TEST_CASE("a locale tag with no language gives nothing", "[base][locale]") {
  CHECK_FALSE(ParseLocaleTag(""));
  CHECK_FALSE(ParseLocaleTag("C"));
  CHECK_FALSE(ParseLocaleTag("C.UTF-8"));
  CHECK_FALSE(ParseLocaleTag("POSIX"));
  CHECK_FALSE(ParseLocaleTag("_US"));
}

REX_TEST_CASE("the locale environment lists LANGUAGE, then LC_ALL, LC_MESSAGES and LANG",
              "[base][locale]") {
  CHECK(Join(ParseLocaleEnvironment("ja_JP:en_GB:fr", "de_DE.UTF-8", "es_ES", "it_IT")) ==
        "ja/JP en/GB fr/ de/DE es/ES it/IT ");
  CHECK(Join(ParseLocaleEnvironment("", "", "", "en_US.UTF-8")) == "en/US ");
  CHECK(Join(ParseLocaleEnvironment("", "C", "", "POSIX")).empty());
  CHECK(Join(ParseLocaleEnvironment("C:ko_KR::", "", "C.UTF-8", "")) == "ko/KR ");
}
