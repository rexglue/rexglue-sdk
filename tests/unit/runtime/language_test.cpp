/**
 * @file        tests/unit/runtime/language_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/api.h>
#include <rex/filesystem/file_system.h>
#include <rex/kernel/init.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/runtime/exports.h>
#include <rex/runtime/language.h>
#include <rex/runtime/thread_state.h>
#include <rex/ui/user_interface.h>

#include "base_config.h"
#include "test_memory.h"
#include "ui/scripted_ui.h"

using rex::runtime::GetLanguageForLocale;
using rex::runtime::GetLanguageForLocales;
using rex::runtime::GetLanguageName;
using rex::runtime::ParseLanguageCode;
using rex::runtime::XLanguage;
using rex::ui::HostLocale;

namespace {

struct Fixture {
  Fixture() {
    rex::test::ReleaseTestMemory();
    rex::Shutdown();
  }
  ~Fixture() { rex::Shutdown(); }
};

XLanguage ForLocale(std::string_view language, std::string_view country = {}) {
  return GetLanguageForLocale(HostLocale{std::string(language), std::string(country)});
}

class BootedRuntime {
 public:
  BootedRuntime(u32 user_language, rex::ui::IUserInterface* ui) {
    rex::Config config = rex::test::BaseConfig();
    config.modules = {"rexfilesystem", "rexruntime"};
    config.runtime.kernel.user_language = user_language;
    auto api = rex::Init(config);
    REQUIRE(api);
    api_ = *api;
    runtime_ = std::make_unique<rex::Runtime>(api_->GetConfig());
    REQUIRE(runtime_->Setup(rex::RuntimeDesc{
        .file_system = api_->Get<rex::filesystem::IFileSystem>(),
        .kernel_init = rex::kernel::InitializeKernel,
        .tool_mode = true,
        .ui = ui,
    }));
    state_ = std::make_unique<rex::runtime::ThreadState>(1, 0x8000, 0, runtime_->GetMemory(),
                                                         runtime_.get());
    previous_ = rex::runtime::ThreadState::Get();
    rex::runtime::ThreadState::Bind(state_.get());
  }

  ~BootedRuntime() {
    rex::runtime::ThreadState::Bind(previous_);
    runtime_->Shutdown();
  }

  rex::Runtime& GetRuntime() { return *runtime_; }

  u32 CallXGetLanguage() {
    const auto table = rex::runtime::GetExports(rex::runtime::ExportModule::kXAM);
    const auto it = std::ranges::find(table, std::string_view("XGetLanguage"),
                                      &rex::runtime::ExportEntry::name);
    REQUIRE(it != table.end());
    REQUIRE(it->fn);
    PPCContext& ctx = *state_->GetContext();
    it->fn(ctx, runtime_->GetMemory()->GetVirtualMembase());
    return ctx.r3.u32;
  }

 private:
  rex::API* api_ = nullptr;
  std::unique_ptr<rex::Runtime> runtime_;
  std::unique_ptr<rex::runtime::ThreadState> state_;
  rex::runtime::ThreadState* previous_ = nullptr;
};

}  // namespace

REX_TEST_CASE("Language: a host locale maps to a title language", "[runtime][language]") {
  CHECK(ForLocale("en", "US") == XLanguage::kEnglish);
  CHECK(ForLocale("en") == XLanguage::kEnglish);
  CHECK(ForLocale("ja", "JP") == XLanguage::kJapanese);
  CHECK(ForLocale("de", "AT") == XLanguage::kGerman);
  CHECK(ForLocale("fr", "CA") == XLanguage::kFrench);
  CHECK(ForLocale("es", "MX") == XLanguage::kSpanish);
  CHECK(ForLocale("it") == XLanguage::kItalian);
  CHECK(ForLocale("ko", "KR") == XLanguage::kKorean);
  CHECK(ForLocale("pt", "BR") == XLanguage::kPortuguese);
  CHECK(ForLocale("pl") == XLanguage::kPolish);
  CHECK(ForLocale("ru", "RU") == XLanguage::kRussian);
  CHECK(ForLocale("EN", "us") == XLanguage::kEnglish);
  CHECK(ForLocale("sv", "SE") == XLanguage::kSwedish);
  CHECK(ForLocale("tr", "TR") == XLanguage::kTurkish);
  CHECK(ForLocale("nb", "NO") == XLanguage::kNorwegian);
  CHECK(ForLocale("no") == XLanguage::kNorwegian);
  CHECK(ForLocale("nl", "NL") == XLanguage::kDutch);
  CHECK(ForLocale("nn", "NO") == XLanguage::kInvalid);
}

REX_TEST_CASE("Language: Chinese goes by the country of the locale", "[runtime][language]") {
  CHECK(ForLocale("zh", "TW") == XLanguage::kTChinese);
  CHECK(ForLocale("zh", "HK") == XLanguage::kTChinese);
  CHECK(ForLocale("zh", "MO") == XLanguage::kTChinese);
  CHECK(ForLocale("zh", "tw") == XLanguage::kTChinese);
  CHECK(ForLocale("zh", "CN") == XLanguage::kSChinese);
  CHECK(ForLocale("zh", "SG") == XLanguage::kSChinese);
  CHECK(ForLocale("zh") == XLanguage::kSChinese);
}

REX_TEST_CASE("Language: a locale with no title language maps to invalid", "[runtime][language]") {
  CHECK(ForLocale("da", "DK") == XLanguage::kInvalid);
  CHECK(ForLocale("xx") == XLanguage::kInvalid);
  CHECK(ForLocale("") == XLanguage::kInvalid);
  CHECK(ForLocale("english") == XLanguage::kInvalid);
  CHECK(ForLocale("us") == XLanguage::kInvalid);
}

REX_TEST_CASE("Language: a list picks the first locale that maps and falls back to English",
              "[runtime][language]") {
  const std::vector<HostLocale> mixed = {{"da", "DK"}, {"", ""}, {"ru", "RU"}, {"ja", "JP"}};
  CHECK(GetLanguageForLocales(mixed) == XLanguage::kRussian);
  const std::vector<HostLocale> unknown = {{"da", "DK"}, {"xx", ""}};
  CHECK(GetLanguageForLocales(unknown) == XLanguage::kEnglish);
  CHECK(GetLanguageForLocales({}) == XLanguage::kEnglish);
}

REX_TEST_CASE("Language: DetectHostLanguage reads the preferred locales of the interface",
              "[runtime][language]") {
  rex::test::ScriptedUI ui;
  CHECK(rex::runtime::DetectHostLanguage(ui) == XLanguage::kEnglish);
  ui.locales = {{"zh", "HK"}, {"en", "US"}};
  CHECK(rex::runtime::DetectHostLanguage(ui) == XLanguage::kTChinese);
  ui.locales = {{"da", "DK"}, {"de", "DE"}};
  CHECK(rex::runtime::DetectHostLanguage(ui) == XLanguage::kGerman);
}

REX_TEST_CASE("Language: names follow the XGetLanguage documentation", "[runtime][language]") {
  CHECK(GetLanguageName(XLanguage::kEnglish) == "English");
  CHECK(GetLanguageName(XLanguage::kJapanese) == "Japanese");
  CHECK(GetLanguageName(XLanguage::kGerman) == "German");
  CHECK(GetLanguageName(XLanguage::kFrench) == "French");
  CHECK(GetLanguageName(XLanguage::kSpanish) == "Spanish");
  CHECK(GetLanguageName(XLanguage::kItalian) == "Italian");
  CHECK(GetLanguageName(XLanguage::kKorean) == "Korean");
  CHECK(GetLanguageName(XLanguage::kTChinese) == "Chinese (Traditional)");
  CHECK(GetLanguageName(XLanguage::kPortuguese) == "Portuguese");
  CHECK(GetLanguageName(XLanguage::kSChinese) == "Chinese (Simplified)");
  CHECK(GetLanguageName(XLanguage::kPolish) == "Polish");
  CHECK(GetLanguageName(XLanguage::kRussian) == "Russian");
  CHECK(GetLanguageName(XLanguage::kSwedish) == "Swedish");
  CHECK(GetLanguageName(XLanguage::kTurkish) == "Turkish");
  CHECK(GetLanguageName(XLanguage::kNorwegian) == "Norwegian");
  CHECK(GetLanguageName(XLanguage::kDutch) == "Dutch");
  CHECK(GetLanguageName(XLanguage::kSChineseDeprecated) ==
        "Placeholder for deprecated constant for Simplified Chinese");
  CHECK(GetLanguageName(XLanguage::kInvalid) == "Unknown");
  CHECK(GetLanguageName(XLanguage::kMaxLanguages) == "Unknown");
}

REX_TEST_CASE("Language: ParseLanguageCode accepts standard tags", "[runtime][language]") {
  CHECK(ParseLanguageCode("en") == XLanguage::kEnglish);
  CHECK(ParseLanguageCode("EN-us") == XLanguage::kEnglish);
  CHECK(ParseLanguageCode("ja") == XLanguage::kJapanese);
  CHECK(ParseLanguageCode("de") == XLanguage::kGerman);
  CHECK(ParseLanguageCode("fr_CA") == XLanguage::kFrench);
  CHECK(ParseLanguageCode("es") == XLanguage::kSpanish);
  CHECK(ParseLanguageCode("it") == XLanguage::kItalian);
  CHECK(ParseLanguageCode("ko") == XLanguage::kKorean);
  CHECK(ParseLanguageCode("pt-BR") == XLanguage::kPortuguese);
  CHECK(ParseLanguageCode("pl") == XLanguage::kPolish);
  CHECK(ParseLanguageCode("ru") == XLanguage::kRussian);
  CHECK(ParseLanguageCode("sv") == XLanguage::kSwedish);
  CHECK(ParseLanguageCode("tr-TR") == XLanguage::kTurkish);
  CHECK(ParseLanguageCode("nb") == XLanguage::kNorwegian);
  CHECK(ParseLanguageCode("no_NO") == XLanguage::kNorwegian);
  CHECK(ParseLanguageCode("nl-BE") == XLanguage::kDutch);
  CHECK(ParseLanguageCode("nn") == XLanguage::kInvalid);
}

REX_TEST_CASE("Language: ParseLanguageCode tells Chinese scripts and regions apart",
              "[runtime][language]") {
  CHECK(ParseLanguageCode("zh") == XLanguage::kSChinese);
  CHECK(ParseLanguageCode("zh-CN") == XLanguage::kSChinese);
  CHECK(ParseLanguageCode("zh-SG") == XLanguage::kSChinese);
  CHECK(ParseLanguageCode("zh-Hans") == XLanguage::kSChinese);
  CHECK(ParseLanguageCode("zh-Hans-CN") == XLanguage::kSChinese);
  CHECK(ParseLanguageCode("zh-TW") == XLanguage::kTChinese);
  CHECK(ParseLanguageCode("zh_hk") == XLanguage::kTChinese);
  CHECK(ParseLanguageCode("zh-MO") == XLanguage::kTChinese);
  CHECK(ParseLanguageCode("zh-Hant") == XLanguage::kTChinese);
  CHECK(ParseLanguageCode("zh-Hant-TW") == XLanguage::kTChinese);
}

REX_TEST_CASE("Language: ParseLanguageCode rejects what is not a standard tag",
              "[runtime][language]") {
  for (const std::string_view code :
       {"", "auto", "english", "us", "jp", "kr", "tw", "cn", "po", "xx", "-", "_US", "e", "enx"}) {
    INFO(code);
    CHECK(ParseLanguageCode(code) == XLanguage::kInvalid);
  }
}

REX_TEST_CASE_METHOD(
    Fixture, "Language: Setup keeps a configured title language and XGetLanguage returns it",
    "[runtime][language]") {
  for (u32 id = 1; id < static_cast<u32>(XLanguage::kMaxLanguages); ++id) {
    INFO(id);
    {
      rex::test::ReleaseTestMemory();
      rex::Shutdown();
    }
    BootedRuntime booted(id, nullptr);
    CHECK(booted.GetRuntime().GetUserLanguage() == static_cast<XLanguage>(id));
    CHECK(booted.CallXGetLanguage() == id);
  }
}

REX_TEST_CASE_METHOD(Fixture, "Language: a language of 0 follows the host", "[runtime][language]") {
  rex::test::ScriptedUI ui;
  ui.locales = {{"da", "DK"}, {"ja", "JP"}};
  {
    BootedRuntime booted(0, &ui);
    CHECK(booted.GetRuntime().GetUserLanguage() == XLanguage::kJapanese);
    CHECK(booted.CallXGetLanguage() == static_cast<u32>(XLanguage::kJapanese));
  }
  rex::test::ReleaseTestMemory();
  rex::Shutdown();
  BootedRuntime without_ui(0, nullptr);
  CHECK(without_ui.GetRuntime().GetUserLanguage() == XLanguage::kEnglish);
  CHECK(without_ui.CallXGetLanguage() == static_cast<u32>(XLanguage::kEnglish));
}

REX_TEST_CASE_METHOD(Fixture, "Language: a value past the title languages becomes English",
                     "[runtime][language]") {
  BootedRuntime booted(static_cast<u32>(XLanguage::kMaxLanguages), nullptr);
  CHECK(booted.GetRuntime().GetUserLanguage() == XLanguage::kEnglish);
  CHECK(booted.CallXGetLanguage() == static_cast<u32>(XLanguage::kEnglish));
}

REX_TEST_CASE(
    "Language: the ids follow the XDK header and host detection never gives the deprecated one",
    "[runtime][language]") {
  STATIC_REQUIRE(static_cast<u32>(XLanguage::kSChineseDeprecated) == 0x0A);
  STATIC_REQUIRE(static_cast<u32>(XLanguage::kSwedish) == 0x0D);
  STATIC_REQUIRE(static_cast<u32>(XLanguage::kTurkish) == 0x0E);
  STATIC_REQUIRE(static_cast<u32>(XLanguage::kNorwegian) == 0x0F);
  STATIC_REQUIRE(static_cast<u32>(XLanguage::kDutch) == 0x10);
  STATIC_REQUIRE(static_cast<u32>(XLanguage::kSChinese) == 0x11);
  for (const char* country : {"", "CN", "SG", "TW", "HK", "MO"}) {
    CHECK(ForLocale("zh", country) != XLanguage::kSChineseDeprecated);
  }
  for (const char* code : {"zh", "zh-CN", "zh-Hans", "zh-TW", "zh-Hant"}) {
    CHECK(ParseLanguageCode(code) != XLanguage::kSChineseDeprecated);
  }
}
