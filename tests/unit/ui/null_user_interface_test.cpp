/**
 * @file        tests/unit/ui/null_user_interface_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include "rex_test.h"

#include <rex/ui/null_user_interface.h>

using rex::u32;
using rex::ui::ChoiceDesc;
using rex::ui::FileDialogDesc;
using rex::ui::NativeWindow;
using rex::ui::NullUserInterface;
using rex::ui::PromptDesc;
using rex::ui::PromptResult;
using rex::ui::UIConfig;
using rex::ui::UIEvent;

namespace {

bool IgnoreEvent(const UIEvent&) {
  return false;
}

}  // namespace

REX_TEST_CASE("OpenWindow returns a window with no native handle", "[ui]") {
  NullUserInterface ui;
  auto window = ui.OpenWindow(UIConfig{}, IgnoreEvent);
  REQUIRE(window);
  REQUIRE(*window != nullptr);
  CHECK(ui.GetWindow() == *window);
  CHECK((*window)->GetNative().kind == NativeWindow::Kind::kNone);
  CHECK((*window)->GetNative().window == nullptr);
  CHECK((*window)->GetNative().sdl == nullptr);
}

REX_TEST_CASE("Prompt completes cancelled at once", "[ui]") {
  NullUserInterface ui;
  bool called = false;
  PromptResult result;
  result.cancelled = false;
  ui.Prompt(PromptDesc{}, [&](PromptResult value) {
    called = true;
    result = std::move(value);
  });
  CHECK(called);
  CHECK(result.cancelled);
}

REX_TEST_CASE("ShowChoice returns the first button", "[ui]") {
  NullUserInterface ui;
  auto choice = ui.ShowChoice(ChoiceDesc{});
  REQUIRE(choice);
  CHECK(*choice == u32{0});
}

REX_TEST_CASE("Post from another thread runs on PumpFrame", "[ui]") {
  NullUserInterface ui;
  bool posted = false;
  bool ran = false;
  std::thread::id ran_on;
  std::thread worker([&] {
    posted = ui.Post([&] {
      ran = true;
      ran_on = std::this_thread::get_id();
    });
  });
  worker.join();
  REQUIRE(posted);
  CHECK_FALSE(ran);
  CHECK(ui.PumpFrame());
  CHECK(ran);
  CHECK(ran_on == std::this_thread::get_id());
}

REX_TEST_CASE("RunSynchronous runs inline on the UI thread and posts from another thread", "[ui]") {
  NullUserInterface ui;
  CHECK(ui.IsUIThread());
  bool inline_ran = false;
  ui.RunSynchronous([&] { inline_ran = true; });
  CHECK(inline_ran);

  std::atomic<bool> other_ran = false;
  std::thread worker([&] {
    CHECK_FALSE(ui.IsUIThread());
    ui.RunSynchronous([&] { other_ran = true; });
  });
  while (!other_ran) {
    ui.PumpFrame();
  }
  worker.join();
  CHECK(other_ran);
}

REX_TEST_CASE("PumpFrame returns false after Quit and after CloseWindow", "[ui]") {
  NullUserInterface quit_ui;
  CHECK(quit_ui.PumpFrame());
  quit_ui.Quit();
  CHECK_FALSE(quit_ui.PumpFrame());

  NullUserInterface close_ui;
  REQUIRE(close_ui.OpenWindow(UIConfig{}, IgnoreEvent));
  CHECK(close_ui.PumpFrame());
  close_ui.CloseWindow();
  CHECK_FALSE(close_ui.PumpFrame());
  CHECK(close_ui.GetWindow() == nullptr);
}

REX_TEST_CASE("Post returns false once the loop quit and RunSynchronous does not wait", "[ui]") {
  NullUserInterface quit_ui;
  quit_ui.Quit();
  CHECK_FALSE(quit_ui.Post([] {}));

  NullUserInterface close_ui;
  REQUIRE(close_ui.OpenWindow(UIConfig{}, IgnoreEvent));
  close_ui.CloseWindow();
  CHECK_FALSE(close_ui.Post([] {}));

  std::atomic<bool> ran = false;
  std::atomic<bool> returned = false;
  std::thread worker([&] {
    quit_ui.RunSynchronous([&] { ran = true; });
    returned = true;
  });
  worker.join();
  CHECK(returned);
  CHECK_FALSE(ran);
}

REX_TEST_CASE("GetDisplays and ShowFileDialog are empty and fonts are null", "[ui]") {
  NullUserInterface ui;
  CHECK(ui.GetDisplays().empty());
  auto files = ui.ShowFileDialog(FileDialogDesc{});
  REQUIRE(files);
  CHECK(files->empty());
  CHECK(ui.GetFonts() == nullptr);
  CHECK(ui.GetImGuiStyle() == nullptr);
  CHECK_FALSE(ui.HasDialogs());
  CHECK_FALSE(ui.IsAnyOverlayVisible());
}

REX_TEST_CASE("GetPreferredLocales on the null provider is empty", "[ui]") {
  NullUserInterface ui;
  CHECK(ui.GetPreferredLocales().empty());
}

REX_TEST_CASE("RunSynchronous work abandoned at Quit never runs", "[ui]") {
  NullUserInterface ui;
  std::atomic<bool> ran{false};
  std::jthread worker([&] {
    bool local = false;
    ui.RunSynchronous([&] {
      local = true;
      ran = true;
    });
    (void)local;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ui.Quit();
  worker.join();
  ui.PumpFrame();
  CHECK_FALSE(ran);
}

REX_TEST_CASE("GetImGuiIO on the null provider is null", "[ui]") {
  NullUserInterface ui;
  CHECK(ui.GetImGuiIO() == nullptr);
}
