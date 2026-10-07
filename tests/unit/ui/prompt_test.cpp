/**
 * @file        tests/unit/ui/prompt_test.cpp
 * @brief       Prompts and message boxes on the ui module
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_video.h>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/module.h>
#include <rex/ui/user_interface.h>

#include "core/cvar_registry.h"
#include "ui/sdl3/prompts.h"
#include "ui/sdl3/video_scope.h"

extern "C" void* rex_create_interface(const char* name, rex::u32 version, rex::i32* out_status);

namespace {

using rex::ui::MessageKind;
using rex::ui::PromptDesc;
using rex::ui::PromptResult;

class TestFactory final : public rex::IInterfaceFactory {
 public:
  [[nodiscard]] rex::Result<void*> Get(std::string_view name, rex::u32 version) override {
    (void)version;
    if (name == rex::cvar::ICvarRegistry::kInterfaceName) {
      return static_cast<void*>(registry);
    }
    return rex::Err<void*>(rex::ErrorCategory::Module, "not served");
  }

  rex::cvar::ICvarRegistry* registry = nullptr;
};

class ModuleFixture {
 public:
  ModuleFixture() : registry_(rex::detail::CreateCvarRegistry()) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    config.ui.video_driver = "dummy";
    config.paths.user_data_root = std::filesystem::temp_directory_path() / "rex_prompt_test";
    factory_.registry = registry_.get();
    rex::i32 status = 0;
    module = static_cast<rex::IModule*>(rex_create_interface("rex.module", 1, &status));
    ui = static_cast<rex::ui::IUserInterface*>(rex_create_interface("rex.ui", 1, &status));
    REQUIRE(module != nullptr);
    REQUIRE(ui != nullptr);
    REQUIRE(module->Connect(&factory_, config));
    REQUIRE(module->Init());
  }

  ~ModuleFixture() {
    module->Shutdown();
    module->Disconnect();
  }

  rex::Config config;
  rex::IModule* module = nullptr;
  rex::ui::IUserInterface* ui = nullptr;

 private:
  std::unique_ptr<rex::detail::CvarRegistryImpl> registry_;
  TestFactory factory_;
};

}  // namespace

TEST_CASE("Prompt without a window completes cancelled at once", "[ui][prompt]") {
  ModuleFixture fixture;
  rex::i32 calls = 0;
  PromptResult got;
  fixture.ui->Prompt(PromptDesc{.title = "Title", .body = "Body"}, [&](PromptResult result) {
    ++calls;
    got = std::move(result);
  });
  CHECK(calls == 1);
  CHECK(got.cancelled);
}

TEST_CASE("Prompt on an open window completes cancelled when the prompts are cancelled",
          "[ui][prompt]") {
  ModuleFixture fixture;
  auto opened =
      fixture.ui->OpenWindow(fixture.config.ui, [](const rex::ui::UIEvent&) { return false; });
  if (!opened) {
    SKIP("SDL dummy video driver unavailable");
  }
  rex::i32 calls = 0;
  PromptResult got;
  fixture.ui->Prompt(PromptDesc{.title = "Title", .body = "Body", .buttons = {"Yes", "No"}},
                     [&](PromptResult result) {
                       ++calls;
                       got = std::move(result);
                     });
  for (rex::i32 frame = 0; frame < 100 && !fixture.ui->HasDialogs(); ++frame) {
    fixture.ui->PumpFrame();
  }
  REQUIRE(fixture.ui->HasDialogs());
  CHECK(calls == 0);
  fixture.ui->CancelPrompts();
  CHECK(calls == 1);
  CHECK(got.cancelled);
  CHECK_FALSE(fixture.ui->HasDialogs());
  fixture.ui->CloseWindow();
}

TEST_CASE("A prompt still queued when the window closes completes cancelled", "[ui][prompt]") {
  ModuleFixture fixture;
  auto opened =
      fixture.ui->OpenWindow(fixture.config.ui, [](const rex::ui::UIEvent&) { return false; });
  if (!opened) {
    SKIP("SDL dummy video driver unavailable");
  }
  rex::i32 calls = 0;
  PromptResult got;
  fixture.ui->Prompt(PromptDesc{.title = "Title", .body = "Body"}, [&](PromptResult result) {
    ++calls;
    got = std::move(result);
  });
  CHECK(calls == 0);
  fixture.ui->CloseWindow();
  CHECK(calls == 1);
  CHECK(got.cancelled);
}

TEST_CASE("A prompt completes once with the pressed button", "[ui][prompt]") {
  rex::i32 calls = 0;
  PromptResult got;
  {
    rex::ui::PromptDialog dialog(PromptDesc{.title = "Title", .buttons = {"Yes", "No"}},
                                 [&](PromptResult result) {
                                   ++calls;
                                   got = std::move(result);
                                 });
    dialog.Choose(1, "");
    CHECK(calls == 1);
    CHECK_FALSE(got.cancelled);
    CHECK(got.button == 1);
  }
  CHECK(calls == 1);
}

TEST_CASE("A text prompt returns the accepted text", "[ui][prompt]") {
  PromptResult got;
  {
    rex::ui::PromptDialog dialog(PromptDesc{.title = "Name", .text_input = true, .max_length = 16},
                                 [&](PromptResult result) { got = std::move(result); });
    dialog.Choose(0, "abc");
  }
  CHECK_FALSE(got.cancelled);
  CHECK(got.button == 0);
  CHECK(got.text == "abc");
}

TEST_CASE("A text prompt holds max_length characters", "[ui][prompt]") {
  rex::ui::PromptDialog dialog(
      PromptDesc{.title = "Name", .text_input = true, .default_text = "abcdef", .max_length = 4},
      [](PromptResult) {});
  CHECK(dialog.Text() == "abcd");
}

TEST_CASE("PreferredLocales returns without a window", "[ui][prompt]") {
  ModuleFixture fixture;
  const std::vector<rex::ui::HostLocale> locales = fixture.ui->PreferredLocales();
  for (const rex::ui::HostLocale& locale : locales) {
    CHECK_FALSE(locale.language.empty());
  }
}

TEST_CASE("A prompt destroyed without a button completes cancelled", "[ui][prompt]") {
  rex::i32 calls = 0;
  PromptResult got;
  {
    rex::ui::PromptDialog dialog(PromptDesc{.title = "Title"}, [&](PromptResult result) {
      ++calls;
      got = std::move(result);
    });
  }
  CHECK(calls == 1);
  CHECK(got.cancelled);
}

TEST_CASE("ShowMessage from a worker thread without a window returns", "[ui][prompt]") {
  ModuleFixture fixture;
  std::promise<void> returned;
  std::future<void> done = returned.get_future();
  std::jthread worker([&] {
    fixture.ui->ShowMessage(MessageKind::Info, "Title", "Body");
    returned.set_value();
  });
  REQUIRE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

TEST_CASE("ShowMessage from a worker thread returns while the window thread is busy",
          "[ui][prompt]") {
  ModuleFixture fixture;
  auto opened =
      fixture.ui->OpenWindow(fixture.config.ui, [](const rex::ui::UIEvent&) { return false; });
  if (!opened) {
    SKIP("SDL dummy video driver unavailable");
  }
  std::promise<void> returned;
  std::future<void> done = returned.get_future();
  std::jthread worker([&] {
    fixture.ui->ShowMessage(MessageKind::Error, "Title", "Body");
    returned.set_value();
  });
  const bool returned_while_busy =
      done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  while (done.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    fixture.ui->PumpFrame();
  }
  CHECK(returned_while_busy);
  fixture.ui->CloseWindow();
}

TEST_CASE("A video scope pins the running video driver", "[ui][prompt]") {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy,offscreen");
  {
    rex::ui::VideoScope video;
    REQUIRE(video.Ok());
    const char* pinned = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    REQUIRE(pinned != nullptr);
    CHECK(std::string(pinned) == std::string(SDL_GetCurrentVideoDriver()));
  }
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
}
