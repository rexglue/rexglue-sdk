/**
 * @file        tests/unit/core/xcontent_language_test.cpp
 * @brief       Package display names and descriptions pick the slot of a title language
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstddef>
#include <memory>
#include <string>

#include <rex/filesystem/xcontent_layout.h>

namespace fs = rex::filesystem;
using rex::runtime::XLanguage;

namespace {

std::unique_ptr<fs::XContentMetadata> MakeMetadata(u8 version) {
  auto metadata = std::make_unique<fs::XContentMetadata>();
  auto* bytes = reinterpret_cast<u8*>(metadata.get());
  bytes[offsetof(fs::XContentMetadata, metadata_version) + 3] = version;
  return metadata;
}

}  // namespace

REX_TEST_CASE("Both Simplified Chinese ids read and write the Simplified Chinese slot",
              "[core][xcontent]") {
  auto metadata = MakeMetadata(2);
  REQUIRE(metadata->SetDisplayName(XLanguage::kSChinese, u"chinese"));
  CHECK(metadata->GetDisplayName(XLanguage::kSChinese) == u"chinese");
  CHECK(metadata->GetDisplayName(XLanguage::kSChineseDeprecated) == u"chinese");
  CHECK(static_cast<char16_t>(metadata->display_name_ex_raw.uint[0][0]) == u'c');
  REQUIRE(metadata->SetDescription(XLanguage::kSChineseDeprecated, u"described"));
  CHECK(metadata->GetDescription(XLanguage::kSChinese) == u"described");
  CHECK(metadata->GetDescription(XLanguage::kSChineseDeprecated) == u"described");
}

REX_TEST_CASE("A language without a slot reads English", "[core][xcontent]") {
  auto metadata = MakeMetadata(2);
  REQUIRE(metadata->SetDisplayName(XLanguage::kEnglish, u"english"));
  REQUIRE(metadata->SetDescription(XLanguage::kEnglish, u"plain"));
  for (const XLanguage language : {XLanguage::kSwedish, XLanguage::kTurkish, XLanguage::kNorwegian,
                                   XLanguage::kDutch, XLanguage::kInvalid}) {
    INFO(static_cast<int>(language));
    CHECK(metadata->GetDisplayName(language) == u"english");
    CHECK(metadata->GetDescription(language) == u"plain");
  }
}

REX_TEST_CASE("Setting a language without a slot fails and leaves English alone",
              "[core][xcontent]") {
  auto metadata = MakeMetadata(2);
  REQUIRE(metadata->SetDisplayName(XLanguage::kEnglish, u"english"));
  REQUIRE(metadata->SetDescription(XLanguage::kEnglish, u"plain"));
  for (const XLanguage language :
       {XLanguage::kSwedish, XLanguage::kTurkish, XLanguage::kNorwegian, XLanguage::kDutch,
        XLanguage::kInvalid, XLanguage::kMaxLanguages}) {
    INFO(static_cast<int>(language));
    CHECK_FALSE(metadata->SetDisplayName(language, u"other"));
    CHECK_FALSE(metadata->SetDescription(language, u"other"));
  }
  CHECK(metadata->GetDisplayName(XLanguage::kEnglish) == u"english");
  CHECK(metadata->GetDescription(XLanguage::kEnglish) == u"plain");
}

REX_TEST_CASE("Version 1 metadata has no Simplified Chinese slot", "[core][xcontent]") {
  auto metadata = MakeMetadata(1);
  CHECK_FALSE(metadata->SetDisplayName(XLanguage::kSChinese, u"chinese"));
  CHECK_FALSE(metadata->SetDescription(XLanguage::kSChineseDeprecated, u"described"));
  CHECK(metadata->GetDisplayName(XLanguage::kSChinese).empty());
  REQUIRE(metadata->SetDisplayName(XLanguage::kJapanese, u"japanese"));
  CHECK(metadata->GetDisplayName(XLanguage::kJapanese) == u"japanese");
}
