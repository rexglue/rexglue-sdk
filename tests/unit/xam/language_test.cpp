/**
 * @file        tests/unit/xam/language_test.cpp
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

#include "kernel/kernel_init.h"
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/export.h>
#include <rex/xam/language.h>
#include <rex/kernel/thread_state.h>
#include <rex/ui/ui_system.h>

#include "test_memory.h"
#include "ui/ui_harness.h"
#include "kernel/title_fixture.h"
#include "kernel/title_state.h"
#include <rex/kernel/types/content.h>

using rex::kernel::XLanguage;
using rex::ui::HostLocale;

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

rex::kernel::XLanguage ForLocale(std::string_view language, std::string_view country = {}) {
  return rex::xam::GetLanguageForLocale(HostLocale{std::string(language), std::string(country)});
}

class BootedRuntime {
 public:
  explicit BootedRuntime(u32 user_language) {
    runtime_ = std::make_unique<rex::kernel::TitleRuntime>(rex::ppc::PPCImage{});
    REQUIRE(runtime_->GetKernelSettings().user_language.Set(user_language));
    REQUIRE(runtime_->Load());
    state_ = std::make_unique<rex::kernel::ThreadState>(
        1, 0x8000, 0, StateOf(*runtime_).GetAddressSpace(), runtime_.get());
    previous_ = rex::kernel::ThreadState::Get();
    rex::kernel::ThreadState::Bind(state_.get());
  }

  ~BootedRuntime() {
    rex::kernel::ThreadState::Bind(previous_);
    runtime_->Unload();
    CHECK(runtime_->GetKernelSettings().user_language.Set(1));
  }

  rex::kernel::TitleRuntime& GetRuntime() { return *runtime_; }

  u32 CallXGetLanguage() {
    const auto table = rex::kernel::GetExports(rex::kernel::ExportModule::kXAM);
    const auto it =
        std::ranges::find(table, std::string_view("XGetLanguage"), &rex::kernel::ExportEntry::name);
    REQUIRE(it != table.end());
    REQUIRE(it->fn);
    PPCContext& ctx = *state_->GetContext();
    it->fn(ctx, StateOf(*runtime_).GetAddressSpace()->GetVirtualMembase());
    return ctx.r3.u32;
  }

 private:
  rex::test::TitleBoot boot_;
  std::unique_ptr<rex::kernel::TitleRuntime> runtime_;
  std::unique_ptr<rex::kernel::ThreadState> state_;
  rex::kernel::ThreadState* previous_ = nullptr;
};

}  // namespace

REX_TEST_CASE("Language: a host locale maps to a title language", "[xam][language]") {
  CHECK(ForLocale("en", "US") == rex::kernel::XLanguage::kEnglish);
  CHECK(ForLocale("en") == rex::kernel::XLanguage::kEnglish);
  CHECK(ForLocale("ja", "JP") == rex::kernel::XLanguage::kJapanese);
  CHECK(ForLocale("de", "AT") == rex::kernel::XLanguage::kGerman);
  CHECK(ForLocale("fr", "CA") == rex::kernel::XLanguage::kFrench);
  CHECK(ForLocale("es", "MX") == rex::kernel::XLanguage::kSpanish);
  CHECK(ForLocale("it") == rex::kernel::XLanguage::kItalian);
  CHECK(ForLocale("ko", "KR") == rex::kernel::XLanguage::kKorean);
  CHECK(ForLocale("pt", "BR") == rex::kernel::XLanguage::kPortuguese);
  CHECK(ForLocale("pl") == rex::kernel::XLanguage::kPolish);
  CHECK(ForLocale("ru", "RU") == rex::kernel::XLanguage::kRussian);
  CHECK(ForLocale("EN", "us") == rex::kernel::XLanguage::kEnglish);
  CHECK(ForLocale("sv", "SE") == rex::kernel::XLanguage::kSwedish);
  CHECK(ForLocale("tr", "TR") == rex::kernel::XLanguage::kTurkish);
  CHECK(ForLocale("nb", "NO") == rex::kernel::XLanguage::kNorwegian);
  CHECK(ForLocale("no") == rex::kernel::XLanguage::kNorwegian);
  CHECK(ForLocale("nl", "NL") == rex::kernel::XLanguage::kDutch);
  CHECK(ForLocale("nn", "NO") == rex::kernel::XLanguage::kInvalid);
}

REX_TEST_CASE("Language: Chinese goes by the country of the locale", "[xam][language]") {
  CHECK(ForLocale("zh", "TW") == rex::kernel::XLanguage::kTChinese);
  CHECK(ForLocale("zh", "HK") == rex::kernel::XLanguage::kTChinese);
  CHECK(ForLocale("zh", "MO") == rex::kernel::XLanguage::kTChinese);
  CHECK(ForLocale("zh", "tw") == rex::kernel::XLanguage::kTChinese);
  CHECK(ForLocale("zh", "CN") == rex::kernel::XLanguage::kSChinese);
  CHECK(ForLocale("zh", "SG") == rex::kernel::XLanguage::kSChinese);
  CHECK(ForLocale("zh") == rex::kernel::XLanguage::kSChinese);
}

REX_TEST_CASE("Language: a locale with no title language maps to invalid", "[xam][language]") {
  CHECK(ForLocale("da", "DK") == rex::kernel::XLanguage::kInvalid);
  CHECK(ForLocale("xx") == rex::kernel::XLanguage::kInvalid);
  CHECK(ForLocale("") == rex::kernel::XLanguage::kInvalid);
  CHECK(ForLocale("english") == rex::kernel::XLanguage::kInvalid);
  CHECK(ForLocale("us") == rex::kernel::XLanguage::kInvalid);
}

REX_TEST_CASE("Language: a list picks the first locale that maps and falls back to English",
              "[xam][language]") {
  const std::vector<HostLocale> mixed = {{"da", "DK"}, {"", ""}, {"ru", "RU"}, {"ja", "JP"}};
  CHECK(rex::xam::GetLanguageForLocales(mixed) == rex::kernel::XLanguage::kRussian);
  const std::vector<HostLocale> unknown = {{"da", "DK"}, {"xx", ""}};
  CHECK(rex::xam::GetLanguageForLocales(unknown) == rex::kernel::XLanguage::kEnglish);
  CHECK(rex::xam::GetLanguageForLocales({}) == rex::kernel::XLanguage::kEnglish);
}

REX_TEST_CASE("Language: DetectHostLanguage reads the preferred locales of the interface",
              "[xam][language]") {
  rex::test_ui::UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  rex::xam::XAMLanguage language;
  CHECK(language.DetectHostLanguage(ui) == rex::kernel::XLanguage::kEnglish);
  rex::test_ui::g_ui.locales = {{"zh", "HK"}, {"en", "US"}};
  CHECK(language.DetectHostLanguage(ui) == rex::kernel::XLanguage::kTChinese);
  rex::test_ui::g_ui.locales = {{"da", "DK"}, {"de", "DE"}};
  CHECK(language.DetectHostLanguage(ui) == rex::kernel::XLanguage::kGerman);
  app.Shutdown();
}

REX_TEST_CASE("Language: ParseLanguageCode accepts standard tags", "[xam][language]") {
  CHECK(rex::xam::ParseLanguageCode("en") == rex::kernel::XLanguage::kEnglish);
  CHECK(rex::xam::ParseLanguageCode("EN-us") == rex::kernel::XLanguage::kEnglish);
  CHECK(rex::xam::ParseLanguageCode("ja") == rex::kernel::XLanguage::kJapanese);
  CHECK(rex::xam::ParseLanguageCode("de") == rex::kernel::XLanguage::kGerman);
  CHECK(rex::xam::ParseLanguageCode("fr_CA") == rex::kernel::XLanguage::kFrench);
  CHECK(rex::xam::ParseLanguageCode("es") == rex::kernel::XLanguage::kSpanish);
  CHECK(rex::xam::ParseLanguageCode("it") == rex::kernel::XLanguage::kItalian);
  CHECK(rex::xam::ParseLanguageCode("ko") == rex::kernel::XLanguage::kKorean);
  CHECK(rex::xam::ParseLanguageCode("pt-BR") == rex::kernel::XLanguage::kPortuguese);
  CHECK(rex::xam::ParseLanguageCode("pl") == rex::kernel::XLanguage::kPolish);
  CHECK(rex::xam::ParseLanguageCode("ru") == rex::kernel::XLanguage::kRussian);
  CHECK(rex::xam::ParseLanguageCode("sv") == rex::kernel::XLanguage::kSwedish);
  CHECK(rex::xam::ParseLanguageCode("tr-TR") == rex::kernel::XLanguage::kTurkish);
  CHECK(rex::xam::ParseLanguageCode("nb") == rex::kernel::XLanguage::kNorwegian);
  CHECK(rex::xam::ParseLanguageCode("no_NO") == rex::kernel::XLanguage::kNorwegian);
  CHECK(rex::xam::ParseLanguageCode("nl-BE") == rex::kernel::XLanguage::kDutch);
  CHECK(rex::xam::ParseLanguageCode("nn") == rex::kernel::XLanguage::kInvalid);
}

REX_TEST_CASE("Language: ParseLanguageCode tells Chinese scripts and regions apart",
              "[xam][language]") {
  CHECK(rex::xam::ParseLanguageCode("zh") == rex::kernel::XLanguage::kSChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-CN") == rex::kernel::XLanguage::kSChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-SG") == rex::kernel::XLanguage::kSChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-Hans") == rex::kernel::XLanguage::kSChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-Hans-CN") == rex::kernel::XLanguage::kSChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-TW") == rex::kernel::XLanguage::kTChinese);
  CHECK(rex::xam::ParseLanguageCode("zh_hk") == rex::kernel::XLanguage::kTChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-MO") == rex::kernel::XLanguage::kTChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-Hant") == rex::kernel::XLanguage::kTChinese);
  CHECK(rex::xam::ParseLanguageCode("zh-Hant-TW") == rex::kernel::XLanguage::kTChinese);
}

REX_TEST_CASE("Language: ParseLanguageCode rejects what is not a standard tag", "[xam][language]") {
  for (const std::string_view code :
       {"", "auto", "english", "us", "jp", "kr", "tw", "cn", "po", "xx", "-", "_US", "e", "enx"}) {
    INFO(code);
    CHECK(rex::xam::ParseLanguageCode(code) == rex::kernel::XLanguage::kInvalid);
  }
}

REX_TEST_CASE_METHOD(Fixture,
                     "Language: Load keeps a configured title language and XGetLanguage returns it",
                     "[xam][language]") {
  for (u32 id = 1; id < static_cast<u32>(rex::kernel::XLanguage::kMaxLanguages); ++id) {
    INFO(id);
    { rex::test::ReleaseTestMemory(); }
    BootedRuntime booted(id);
    CHECK(booted.GetRuntime().GetUserLanguage() == static_cast<XLanguage>(id));
    CHECK(booted.CallXGetLanguage() == id);
  }
}

REX_TEST_CASE_METHOD(Fixture, "Language: a language of 0 without a UI is English",
                     "[xam][language]") {
  BootedRuntime without_ui(0);
  CHECK(without_ui.GetRuntime().GetUserLanguage() == rex::kernel::XLanguage::kEnglish);
  CHECK(without_ui.CallXGetLanguage() == static_cast<u32>(rex::kernel::XLanguage::kEnglish));
}

REX_TEST_CASE_METHOD(Fixture, "Language: a value past the title languages becomes English",
                     "[xam][language]") {
  BootedRuntime booted(static_cast<u32>(rex::kernel::XLanguage::kMaxLanguages));
  CHECK(booted.GetRuntime().GetUserLanguage() == rex::kernel::XLanguage::kEnglish);
  CHECK(booted.CallXGetLanguage() == static_cast<u32>(rex::kernel::XLanguage::kEnglish));
}

REX_TEST_CASE(
    "Language: the ids follow the XDK header and host detection never gives the deprecated one",
    "[xam][language]") {
  for (const char* country : {"", "CN", "SG", "TW", "HK", "MO"}) {
    CHECK(ForLocale("zh", country) != rex::kernel::XLanguage::kSChineseDeprecated);
  }
  for (const char* code : {"zh", "zh-CN", "zh-Hans", "zh-TW", "zh-Hant"}) {
    CHECK(rex::xam::ParseLanguageCode(code) != rex::kernel::XLanguage::kSChineseDeprecated);
  }
}
