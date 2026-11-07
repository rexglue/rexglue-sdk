/**
 * @file        tests/unit/ui/ui_system_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/hid/input_system.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/logging/logger.h>
#include <rex/ui/ui_system.h>
#include <rex/overlay_renderer.h>
#include <rex/ui/dialog.h>
#include <rex/ui/ui_event.h>

#include "base/driver_context.h"
#include "ui_harness.h"

using rex::test_ui::g_ui;
using rex::test_ui::UIApp;

namespace {

class CountingListener final : public rex::ui::IUIEventListener {
 public:
  void OnUIEvent(const rex::ui::UIEvent&) override {}
  void OnInputEvent(rex::InputEvent& event) override {
    if (event.type == rex::InputEvent::Type::kKeyDown) {
      ++key_downs;
    }
    if (event.type == rex::InputEvent::Type::kWheel) {
      wheel_turns.push_back(event.scroll_y);
    }
  }
  int key_downs = 0;
  std::vector<rex::f32> wheel_turns;
};

class TextBoxDialog final : public rex::ui::DialogBase {
 public:
  bool Draw(ImGuiIO&) override {
    ImGui::Begin("text box");
    ImGui::SetKeyboardFocusHere();
    ImGui::InputText("text", text, sizeof(text));
    ImGui::End();
    return true;
  }

 private:
  char text[32] = {};
};

class ClosingDialog final : public rex::ui::DialogBase {
 public:
  ClosingDialog(int& closed, int& destroyed) : closed_(closed), destroyed_(destroyed) {}
  ~ClosingDialog() override { ++destroyed_; }

  bool Draw(ImGuiIO&) override { return false; }
  void OnClose() override { ++closed_; }

 private:
  int& closed_;
  int& destroyed_;
};

class NullRenderer final : public rex::IOverlayRenderer {
 public:
  void RenderOverlays(ImDrawData& draw_data) override {
    for (ImTextureData* texture : *draw_data.Textures) {
      if (texture->Status == ImTextureStatus_WantDestroy) {
        texture->SetStatus(ImTextureStatus_Destroyed);
      } else if (texture->Status != ImTextureStatus_OK) {
        texture->SetTexID(static_cast<ImTextureID>(1));
        texture->SetStatus(ImTextureStatus_OK);
      }
    }
  }
};

void Frames(UIApp& app, rex::ui::IUISystem& ui, NullRenderer& renderer, int count) {
  for (int i = 0; i < count; ++i) {
    ui.DrawOverlays(renderer);
    app.Pump();
  }
}

void DeliverButton(rex::InputEvent::Type type) {
  rex::test_ui::DeliverMouseButton(type, rex::MouseButton::kLeft);
}

void RunWithCallerContext(ImGuiContext* caller) {
  ImGui::SetCurrentContext(caller);
  {
    UIApp app;
    REQUIRE(app.Start());
    rex::ui::IUISystem& ui = app.GetUI();
    CHECK(ImGui::GetCurrentContext() == caller);
    NullRenderer renderer;
    ui.ShowPrompt({.title = "Title", .body = "Body"}, [](rex::ui::PromptResult) {});
    Frames(app, ui, renderer, 3);
    REQUIRE(ui.HasDialogs());
    CHECK(ImGui::GetCurrentContext() == caller);
    DeliverButton(rex::InputEvent::Type::kButtonDown);
    app.Pump();
    CHECK(ImGui::GetCurrentContext() == caller);
    DeliverButton(rex::InputEvent::Type::kButtonUp);
    app.Pump();
    CHECK(ImGui::GetCurrentContext() == caller);
    ui.CancelPrompts();
    CHECK(ImGui::GetCurrentContext() == caller);
    app.Shutdown();
    CHECK(ImGui::GetCurrentContext() == caller);
  }
  ImGui::SetCurrentContext(nullptr);
}

void DeliverMotion(rex::i32 x, rex::i32 y, rex::f32 dx, rex::f32 dy) {
  rex::test_ui::Deliver([=](rex::ui::IUIEventListener& listener) {
    rex::InputEvent event{.type = rex::InputEvent::Type::kMotion,
                          .x = static_cast<rex::f32>(x),
                          .y = static_cast<rex::f32>(y),
                          .dx = dx,
                          .dy = dy};
    listener.OnInputEvent(event);
  });
}

}  // namespace

REX_TEST_CASE("API serves the UI system only while the app runs", "[ui]") {
  UIApp app;
  CHECK(REX_INTERFACE(rex::ui::IUISystem) == nullptr);
  REQUIRE(app.Start());
  CHECK(REX_INTERFACE(rex::ui::IUISystem) == &app.GetUI());
  app.Shutdown();
  CHECK(REX_INTERFACE(rex::ui::IUISystem) == nullptr);
}

REX_TEST_CASE("headless loop runs", "[ui]") {
  UIApp app(false);
  app.configure = [](rex::ui::IUISystem& ui) {
    (void)ui.GetSettings().video_mode_width.SetText(rex::SettingLayer::kTitle, "1920");
    (void)ui.GetSettings().video_mode_height.SetText(rex::SettingLayer::kTitle, "1080");
  };
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  CHECK(ui.IsHeadless());
  CHECK_FALSE(ui.HasWindow());
  CHECK_FALSE(ui.GetNativeWindow());
  int ran = 0;
  ui.PostTask([&ran] { ++ran; });
  CHECK(ran == 0);
  CHECK(app.Pump());
  CHECK(ran == 1);
  CHECK(ui.GetVideoMode().width == 1920);
  CHECK(ui.GetVideoMode().height == 1080);
  app.Shutdown();
}

REX_TEST_CASE("message box logs when headless", "[ui]") {
  UIApp app(false);
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  ui.ShowMessage(rex::MessageType::kError, "Title", "Body");
  CHECK(g_ui.boxes.empty());
  rex::ui::ChoiceConfig choice;
  choice.title = "Title";
  choice.body = "Body";
  CHECK_FALSE(ui.ShowChoice(choice));
  CHECK(g_ui.boxes.empty());
  app.Shutdown();
}

REX_TEST_CASE("prompt cancels with no window", "[ui]") {
  UIApp app(false);
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  int calls = 0;
  bool canceled = false;
  ui.ShowPrompt({.title = "Prompt", .body = "Body"}, [&](rex::ui::PromptResult result) {
    ++calls;
    canceled = result.canceled;
  });
  app.Pump();
  CHECK(calls == 1);
  CHECK(canceled);
  app.Shutdown();
}

REX_TEST_CASE("window settings apply live", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  REQUIRE(ui.HasWindow());
  CHECK(g_ui.fullscreen);
  const int before = g_ui.set_fullscreen_calls;
  REQUIRE(ui.GetSettings().fullscreen.SetText(rex::SettingLayer::kRuntime, "false"));
  app.Pump();
  CHECK(g_ui.set_fullscreen_calls == before + 1);
  CHECK_FALSE(g_ui.fullscreen);
  CHECK(g_ui.thread_of_last_call == std::this_thread::get_id());
  REQUIRE(ui.GetSettings().title.SetText(rex::SettingLayer::kRuntime, "Renamed"));
  app.Pump();
  CHECK(g_ui.title == "Renamed");
  app.Shutdown();
}

REX_TEST_CASE("events reach input and listeners", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  CountingListener listener;
  auto listener_guard = ui.AddEventListener(listener);
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kA);
  app.Pump();
  CHECK(app.GetInput().IsKeyDown(rex::VirtualKey::kA));
  CHECK(listener.key_downs == 1);
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyUp, rex::VirtualKey::kA);
  app.Pump();
  CHECK_FALSE(app.GetInput().IsKeyDown(rex::VirtualKey::kA));
  listener_guard.Reset();
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kB);
  app.Pump();
  CHECK(listener.key_downs == 1);
  app.Shutdown();
}

REX_TEST_CASE("dropping the guard unsubscribes the event listener", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  CountingListener listener;
  { auto listener_guard = app.GetUI().AddEventListener(listener); }
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kA);
  app.Pump();
  CHECK(listener.key_downs == 0);
  app.Shutdown();
}

REX_TEST_CASE("a wheel turn keeps its fraction for listeners and truncates for input", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  CountingListener listener;
  auto listener_guard = ui.AddEventListener(listener);
  rex::test_ui::DeliverWheelTurn(60.0f / 120.0f);
  app.Pump();
  CHECK(app.GetInput().TakeWheel() == 0);
  rex::test_ui::DeliverWheelTurn(1.0f);
  app.Pump();
  CHECK(app.GetInput().TakeWheel() == 1);
  rex::test_ui::DeliverWheelTurn(-60.0f / 120.0f);
  app.Pump();
  CHECK(app.GetInput().TakeWheel() == 0);
  rex::test_ui::DeliverWheelTurn(239.0f / 120.0f);
  app.Pump();
  CHECK(app.GetInput().TakeWheel() == 1);
  rex::test_ui::DeliverWheelTurn(-239.0f / 120.0f);
  app.Pump();
  CHECK(app.GetInput().TakeWheel() == -1);
  listener_guard.Reset();
  REQUIRE(listener.wheel_turns.size() == 5);
  CHECK(listener.wheel_turns[0] == 0.5f);
  CHECK(listener.wheel_turns[1] == 1.0f);
  CHECK(listener.wheel_turns[2] == -0.5f);
  CHECK(listener.wheel_turns[3] == 239.0f / 120.0f);
  CHECK(listener.wheel_turns[4] == -239.0f / 120.0f);
  app.Shutdown();
}

REX_TEST_CASE("a lost focus releases keys", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kC);
  app.Pump();
  REQUIRE(app.GetInput().IsKeyDown(rex::VirtualKey::kC));
  rex::test_ui::DeliverFocus(false);
  app.Pump();
  CHECK_FALSE(app.GetUI().IsFocused());
  CHECK_FALSE(app.GetInput().IsKeyDown(rex::VirtualKey::kC));
  rex::test_ui::DeliverFocus(true);
  app.Pump();
  CHECK(app.GetUI().IsFocused());
  app.Shutdown();
}

REX_TEST_CASE("a post from another thread wakes the driver", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  const int before = g_ui.wake_calls;
  std::atomic<bool> posted{false};
  std::thread poster([&] { ui.PostTask([&posted] { posted = true; }); });
  poster.join();
  CHECK(g_ui.wake_calls > before);
  app.Pump();
  CHECK(posted.load());
  app.Shutdown();
}

REX_TEST_CASE("a locked pointer mode reports motion", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  app.GetInput().SetPointerMode(rex::PointerMode::kLocked);
  app.Pump();
  CHECK(g_ui.pointer_mode == rex::PointerMode::kLocked);
  DeliverMotion(100, 100, 5.0f, -3.0f);
  app.Pump();
  const rex::PointerDelta motion = app.GetInput().TakeMotion();
  CHECK(motion.dx == 5.0f);
  CHECK(motion.dy == -3.0f);
  app.GetInput().SetPointerMode(rex::PointerMode::kFree);
  app.Pump();
  CHECK(g_ui.pointer_mode == rex::PointerMode::kFree);
  app.Shutdown();
}

REX_TEST_CASE("an overlay with keyboard focus hides keys from the title", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  CountingListener listener;
  auto listener_guard = ui.AddEventListener(listener);
  TextBoxDialog dialog;
  auto dialog_guard = ui.AddDialog(dialog);
  NullRenderer renderer;
  for (int i = 0; i < 3; ++i) {
    ui.DrawOverlays(renderer);
    app.Pump();
  }
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kA);
  app.Pump();
  CHECK(listener.key_downs == 0);
  CHECK_FALSE(app.GetInput().IsKeyDown(rex::VirtualKey::kA));
  dialog_guard.Reset();
  ui.DrawOverlays(renderer);
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kB);
  app.Pump();
  CHECK(listener.key_downs == 1);
  CHECK(app.GetInput().IsKeyDown(rex::VirtualKey::kB));
  listener_guard.Reset();
  app.Shutdown();
}

REX_TEST_CASE("a dropped dialog guard removes the dialog", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  TextBoxDialog dialog;
  {
    auto dialog_guard = ui.AddDialog(dialog);
    CHECK(ui.HasDialogs());
  }
  CHECK_FALSE(ui.HasDialogs());
  app.Shutdown();
}

REX_TEST_CASE("an old dialog guard does not remove a newer dialog at the same address", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  static_assert(sizeof(ClosingDialog) <= 256 && sizeof(TextBoxDialog) <= 256);
  alignas(std::max_align_t) unsigned char storage[256];
  int closed = 0;
  int destroyed = 0;
  auto* first = new (storage) ClosingDialog(closed, destroyed);
  rex::ScopeGuard old_guard = ui.AddDialog(*first);
  NullRenderer renderer;
  ui.DrawOverlays(renderer);
  REQUIRE_FALSE(ui.HasDialogs());
  first->~ClosingDialog();
  auto* second = new (storage) TextBoxDialog();
  REQUIRE(static_cast<void*>(second) == static_cast<void*>(first));
  rex::ScopeGuard new_guard = ui.AddDialog(*second);
  old_guard.Reset();
  CHECK(ui.HasDialogs());
  new_guard.Reset();
  CHECK_FALSE(ui.HasDialogs());
  second->~TextBoxDialog();
  app.Shutdown();
}

REX_TEST_CASE("a host-owned dialog is destroyed when it closes", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  int closed = 0;
  int destroyed = 0;
  ui.AddDialog(std::make_unique<ClosingDialog>(closed, destroyed));
  CHECK(ui.HasDialogs());
  NullRenderer renderer;
  for (int i = 0; i < 5 && ui.HasDialogs(); ++i) {
    ui.DrawOverlays(renderer);
    app.Pump();
  }
  CHECK_FALSE(ui.HasDialogs());
  CHECK(closed == 1);
  CHECK(destroyed == 1);
  app.Shutdown();
}

REX_TEST_CASE("a posted function that throws is logged and later work runs", "[ui]") {
  UIApp app(false);
  REQUIRE(app.Start());
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  bool later = false;
  app.GetUI().PostTask([] { throw std::runtime_error("pending function failed"); });
  app.GetUI().PostTask([&later] { later = true; });
  app.Pump();
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  CHECK(std::ranges::any_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.text.find("posted work failed, pending function failed") != std::string::npos;
  }));
  CHECK(later);
  app.Shutdown();
}

REX_TEST_CASE("a post to the UI thread from a worker returns while the UI thread is busy", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::IUIThread* ui = REX_INTERFACE(rex::IUIThread);
  REQUIRE(ui != nullptr);
  rex::AppDriverContext context(REX_SETTINGS(), REX_LOGGER());
  context.SetUI(ui);
  std::atomic<bool> ran{false};
  std::atomic<bool> returned{false};
  std::thread worker([&] {
    context.PostToUIThread([&ran] { ran = true; });
    returned = true;
  });
  worker.join();
  CHECK(returned.load());
  CHECK_FALSE(ran.load());
  app.Pump();
  CHECK(ran.load());
  context.SetUI(nullptr);
  app.Shutdown();
}

REX_TEST_CASE("RunSynchronous from a worker after Shutdown does not run and does not wait",
              "[ui]") {
  UIApp app(false);
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  app.Shutdown();
  bool ran = false;
  bool accepted = true;
  std::thread worker([&] { accepted = ui.RunSynchronous([&ran] { ran = true; }); });
  worker.join();
  CHECK_FALSE(accepted);
  CHECK_FALSE(ran);
}

REX_TEST_CASE("the preferred locales are empty without a window", "[ui]") {
  UIApp app(false);
  REQUIRE(app.Start());
  g_ui.locales = {{"ja", "JP"}};
  CHECK(app.GetUI().GetPreferredLocales().empty());
  app.Shutdown();
}

REX_TEST_CASE("a message from a worker thread returns without the UI thread running", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  std::thread worker([&] { ui.ShowMessage(rex::MessageType::kInfo, "Title", "Body"); });
  worker.join();
  CHECK(g_ui.boxes.size() == 1);
  app.Shutdown();
}

REX_TEST_CASE("a message from a worker thread without a window returns", "[ui]") {
  UIApp app(false);
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  std::thread worker([&] { ui.ShowMessage(rex::MessageType::kInfo, "Title", "Body"); });
  worker.join();
  CHECK(g_ui.boxes.empty());
  app.Shutdown();
}

REX_TEST_CASE("a prompt on an open window completes cancelled when the prompts are cancelled",
              "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  NullRenderer renderer;
  int calls = 0;
  bool canceled = false;
  ui.ShowPrompt({.title = "Prompt", .body = "Body"}, [&](rex::ui::PromptResult result) {
    ++calls;
    canceled = result.canceled;
  });
  Frames(app, ui, renderer, 3);
  CHECK(ui.HasDialogs());
  CHECK(calls == 0);
  ui.CancelPrompts();
  CHECK(calls == 1);
  CHECK(canceled);
  app.Shutdown();
}

REX_TEST_CASE("a prompt pending at Shutdown completes cancelled", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  NullRenderer renderer;
  int calls = 0;
  bool canceled = false;
  ui.ShowPrompt({.title = "Prompt", .body = "Body"}, [&](rex::ui::PromptResult result) {
    ++calls;
    canceled = result.canceled;
  });
  Frames(app, ui, renderer, 3);
  app.Shutdown();
  CHECK(calls == 1);
  CHECK(canceled);
}

REX_TEST_CASE("a text prompt returns the accepted text", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  NullRenderer renderer;
  int calls = 0;
  rex::ui::PromptResult answer;
  ui.ShowPrompt({.title = "Name", .text_input = true, .default_text = "abc"},
                [&](rex::ui::PromptResult result) {
                  ++calls;
                  answer = std::move(result);
                });
  Frames(app, ui, renderer, 4);
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kReturn);
  app.Pump();
  Frames(app, ui, renderer, 2);
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyUp, rex::VirtualKey::kReturn);
  app.Pump();
  Frames(app, ui, renderer, 2);
  REQUIRE(calls == 1);
  CHECK_FALSE(answer.canceled);
  CHECK(answer.button == 0);
  CHECK(answer.text == "abc");
  app.Shutdown();
}

REX_TEST_CASE("with no ImGui context current the window opens, takes input and closes",
              "[ui][imgui]") {
  RunWithCallerContext(nullptr);
}

REX_TEST_CASE("with another ImGui context current the window leaves it current", "[ui][imgui]") {
  ImGuiContext* caller = ImGui::CreateContext();
  RunWithCallerContext(caller);
  ImGui::DestroyContext(caller);
}

REX_TEST_CASE("Shutdown joins a worker that waits on the UI thread with a dialog open", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  NullRenderer renderer;
  ui.ShowPrompt({.title = "Prompt", .body = "Body"}, [](rex::ui::PromptResult) {});
  Frames(app, ui, renderer, 3);
  REQUIRE(ui.HasDialogs());
  std::atomic<bool> running{true};
  std::atomic<bool> entered{false};
  std::atomic<bool> finished{false};
  std::thread worker([&] {
    NullRenderer worker_renderer;
    while (running.load()) {
      entered = true;
      ui.DrawOverlays(worker_renderer);
    }
    finished = true;
  });
  while (!entered.load()) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  app.on_release = [&] {
    running = false;
    while (!finished.load() && app.GetUIControl()->ServePosts(std::chrono::milliseconds(2))) {}
    worker.join();
  };
  app.Shutdown();
  CHECK_FALSE(worker.joinable());
}

REX_TEST_CASE("a worker in RunSynchronous runs its work on the UI thread while Shutdown waits",
              "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  const std::thread::id ui_thread = std::this_thread::get_id();
  std::atomic<bool> entered{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> ran{false};
  std::thread::id ran_on;
  bool accepted = false;
  std::thread worker([&] {
    entered = true;
    accepted = ui.RunSynchronous([&] {
      ran_on = std::this_thread::get_id();
      ran = true;
    });
    finished = true;
  });
  while (!entered.load()) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  app.on_release = [&] {
    while (!finished.load() && app.GetUIControl()->ServePosts(std::chrono::milliseconds(2))) {}
    worker.join();
  };
  app.Shutdown();
  CHECK(accepted);
  CHECK(ran.load());
  CHECK(ran_on == ui_thread);
}

REX_TEST_CASE("RunSynchronous after the UI system stopped returns without running", "[ui]") {
  UIApp app;
  REQUIRE(app.Start());
  rex::ui::IUISystem& ui = app.GetUI();
  app.Shutdown();
  bool ran = false;
  CHECK_FALSE(ui.RunSynchronous([&] { ran = true; }));
  CHECK_FALSE(ran);
  std::thread other([&] { CHECK_FALSE(ui.RunSynchronous([&] { ran = true; })); });
  other.join();
  CHECK_FALSE(ran);
}

REX_TEST_CASE("the UI starts without an input system and shows bindings as text", "[ui][bind]") {
  UIApp app;
  app.without_input = true;
  REQUIRE(app.Start());
  auto found = REX_SETTINGS().Find("ui.bind.console");
  REQUIRE(found);
  CHECK((*found)->GetText().empty());
  REQUIRE(REX_SETTINGS().SetText("ui.bind.console", "Ctrl+K"));
  CHECK((*found)->GetText() == "Ctrl+K");
  CHECK(app.Pump());
  app.Shutdown();
}
