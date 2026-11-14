/**
 * @file        tests/unit/base/xcontent_language_test.cpp
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

#include <rex/filesystem/stfs.h>
#include <rex/kernel/types/content.h>

namespace fs = rex::filesystem;
using rex::kernel::XLanguage;

namespace {

std::unique_ptr<fs::XContentMetadata> MakeMetadata(u8 version) {
  auto metadata = std::make_unique<fs::XContentMetadata>();
  auto* bytes = reinterpret_cast<u8*>(metadata.get());
  bytes[offsetof(fs::XContentMetadata, metadata_version) + 3] = version;
  return metadata;
}

}  // namespace

REX_TEST_CASE("Both Simplified Chinese ids read and write the Simplified Chinese slot",
              "[base][xcontent]") {
  auto metadata = MakeMetadata(2);
  REQUIRE(metadata->SetDisplayName(rex::kernel::XLanguage::kSChinese, u"chinese"));
  CHECK(metadata->GetDisplayName(rex::kernel::XLanguage::kSChinese) == u"chinese");
  CHECK(metadata->GetDisplayName(rex::kernel::XLanguage::kSChineseDeprecated) == u"chinese");
  CHECK(static_cast<char16_t>(metadata->display_name_ex_raw.uint[0][0]) == u'c');
  REQUIRE(metadata->SetDescription(rex::kernel::XLanguage::kSChineseDeprecated, u"translated"));
  CHECK(metadata->GetDescription(rex::kernel::XLanguage::kSChinese) == u"translated");
  CHECK(metadata->GetDescription(rex::kernel::XLanguage::kSChineseDeprecated) == u"translated");
}

REX_TEST_CASE("A language without a slot reads English", "[base][xcontent]") {
  auto metadata = MakeMetadata(2);
  REQUIRE(metadata->SetDisplayName(rex::kernel::XLanguage::kEnglish, u"english"));
  REQUIRE(metadata->SetDescription(rex::kernel::XLanguage::kEnglish, u"plain"));
  for (const rex::kernel::XLanguage language :
       {rex::kernel::XLanguage::kSwedish, rex::kernel::XLanguage::kTurkish,
        rex::kernel::XLanguage::kNorwegian, rex::kernel::XLanguage::kDutch,
        rex::kernel::XLanguage::kInvalid}) {
    INFO(static_cast<int>(language));
    CHECK(metadata->GetDisplayName(language) == u"english");
    CHECK(metadata->GetDescription(language) == u"plain");
  }
}

REX_TEST_CASE("Setting a language without a slot fails and leaves English alone",
              "[base][xcontent]") {
  auto metadata = MakeMetadata(2);
  REQUIRE(metadata->SetDisplayName(rex::kernel::XLanguage::kEnglish, u"english"));
  REQUIRE(metadata->SetDescription(rex::kernel::XLanguage::kEnglish, u"plain"));
  for (const rex::kernel::XLanguage language :
       {rex::kernel::XLanguage::kSwedish, rex::kernel::XLanguage::kTurkish,
        rex::kernel::XLanguage::kNorwegian, rex::kernel::XLanguage::kDutch,
        rex::kernel::XLanguage::kInvalid, rex::kernel::XLanguage::kMaxLanguages}) {
    INFO(static_cast<int>(language));
    CHECK_FALSE(metadata->SetDisplayName(language, u"other"));
    CHECK_FALSE(metadata->SetDescription(language, u"other"));
  }
  CHECK(metadata->GetDisplayName(rex::kernel::XLanguage::kEnglish) == u"english");
  CHECK(metadata->GetDescription(rex::kernel::XLanguage::kEnglish) == u"plain");
}

REX_TEST_CASE("Version 1 metadata has no Simplified Chinese slot", "[base][xcontent]") {
  auto metadata = MakeMetadata(1);
  CHECK_FALSE(metadata->SetDisplayName(rex::kernel::XLanguage::kSChinese, u"chinese"));
  CHECK_FALSE(metadata->SetDescription(rex::kernel::XLanguage::kSChineseDeprecated, u"translated"));
  CHECK(metadata->GetDisplayName(rex::kernel::XLanguage::kSChinese).empty());
  REQUIRE(metadata->SetDisplayName(rex::kernel::XLanguage::kJapanese, u"japanese"));
  CHECK(metadata->GetDisplayName(rex::kernel::XLanguage::kJapanese) == u"japanese");
}
