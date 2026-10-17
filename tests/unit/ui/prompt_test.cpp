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

#include "rex_test.h"
#include "support/temp_folder.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <SDL3/SDL_events.h>
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
    return rex::Err<void*>(rex::ErrorCategory::kModule, "not served");
  }

  rex::cvar::ICvarRegistry* registry = nullptr;
};

class ModuleFixture {
 public:
  ModuleFixture() : registry_(rex::detail::CreateCvarRegistry()) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    config.ui.video_driver = "dummy";
    config.paths.user_data_root = user_root_.path;
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
  rex::test::TempFolder user_root_{"rex_prompt_test_"};
  std::unique_ptr<rex::detail::CvarRegistryImpl> registry_;
  TestFactory factory_;
};

}  // namespace

REX_TEST_CASE("Prompt without a window completes cancelled at once", "[ui][prompt]") {
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

REX_TEST_CASE("Prompt on an open window completes cancelled when the prompts are cancelled",
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

REX_TEST_CASE("A prompt still queued when the window closes completes cancelled", "[ui][prompt]") {
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

REX_TEST_CASE("A prompt completes once with the pressed button", "[ui][prompt]") {
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

REX_TEST_CASE("A text prompt returns the accepted text", "[ui][prompt]") {
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

REX_TEST_CASE("A text prompt holds max_length characters", "[ui][prompt]") {
  rex::ui::PromptDialog dialog(
      PromptDesc{.title = "Name", .text_input = true, .default_text = "abcdef", .max_length = 4},
      [](const PromptResult&) {});
  CHECK(dialog.GetText() == "abcd");
}

REX_TEST_CASE("GetPreferredLocales returns without a window", "[ui][prompt]") {
  ModuleFixture fixture;
  const std::vector<rex::ui::HostLocale> locales = fixture.ui->GetPreferredLocales();
  for (const rex::ui::HostLocale& locale : locales) {
    CHECK_FALSE(locale.language.empty());
  }
}

REX_TEST_CASE("A prompt destroyed without a button completes cancelled", "[ui][prompt]") {
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

REX_TEST_CASE("ShowMessage from a worker thread without a window returns", "[ui][prompt]") {
  ModuleFixture fixture;
  std::promise<void> returned;
  std::future<void> done = returned.get_future();
  std::jthread worker([&] {
    fixture.ui->ShowMessage(MessageKind::kInfo, "Title", "Body");
    returned.set_value();
  });
  REQUIRE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

REX_TEST_CASE("ShowMessage from a worker thread returns while the window thread is busy",
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
    fixture.ui->ShowMessage(MessageKind::kError, "Title", "Body");
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

REX_TEST_CASE("A video scope pins the running video driver", "[ui][prompt]") {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy,offscreen");
  {
    rex::ui::VideoScope video;
    REQUIRE(video.IsOk());
    const char* pinned = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    REQUIRE(pinned != nullptr);
    CHECK(std::string(pinned) == std::string(SDL_GetCurrentVideoDriver()));
  }
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
}

namespace {

void PushMouse(SDL_WindowID window, SDL_EventType type) {
  SDL_Event event{};
  event.button.type = type;
  event.button.windowID = window;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.clicks = 1;
  event.button.x = 4.0f;
  event.button.y = 4.0f;
  SDL_PushEvent(&event);
}

void DriveDrawerWithCaller(ImGuiContext* caller) {
  ImGui::SetCurrentContext(caller);
  {
    ModuleFixture fixture;
    auto opened =
        fixture.ui->OpenWindow(fixture.config.ui, [](const rex::ui::UIEvent&) { return false; });
    if (!opened) {
      ImGui::SetCurrentContext(nullptr);
      SKIP("SDL dummy video driver unavailable");
    }
    CHECK(ImGui::GetCurrentContext() == caller);
    CHECK(fixture.ui->GetImGuiStyle() != nullptr);
    CHECK(ImGui::GetCurrentContext() == caller);
    fixture.ui->Prompt(PromptDesc{.title = "Title", .body = "Body"}, [](PromptResult) {});
    for (rex::i32 frame = 0; frame < 100 && !fixture.ui->HasDialogs(); ++frame) {
      fixture.ui->PumpFrame();
    }
    REQUIRE(fixture.ui->HasDialogs());
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    REQUIRE(windows != nullptr);
    REQUIRE(count > 0);
    const SDL_WindowID window = SDL_GetWindowID(windows[0]);
    SDL_free(windows);
    PushMouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN);
    fixture.ui->PumpFrame();
    CHECK(ImGui::GetCurrentContext() == caller);
    PushMouse(window, SDL_EVENT_MOUSE_BUTTON_UP);
    fixture.ui->PumpFrame();
    CHECK(ImGui::GetCurrentContext() == caller);
    fixture.ui->CancelPrompts();
    CHECK(ImGui::GetCurrentContext() == caller);
    fixture.ui->CloseWindow();
    CHECK(ImGui::GetCurrentContext() == caller);
  }
  ImGui::SetCurrentContext(nullptr);
}

}  // namespace

REX_TEST_CASE("With no ImGui context current the window opens, takes input and closes",
              "[ui][imgui]") {
  DriveDrawerWithCaller(nullptr);
}

REX_TEST_CASE("With another ImGui context current the window leaves it current", "[ui][imgui]") {
  ImGuiContext* caller = ImGui::CreateContext();
  DriveDrawerWithCaller(caller);
  ImGui::DestroyContext(caller);
}
