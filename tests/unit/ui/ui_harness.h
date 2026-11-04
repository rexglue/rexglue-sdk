/**
 * @file        tests/unit/ui/ui_harness.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "object_app.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/apu/audio_system.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/ui/native_window.h>
#include <rex/cli.h>
#include <rex/hid/input_system.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "ui/ui_control.h"

#include "../hid/input_harness.h"
#include "temp_folder.h"

namespace rex::test_ui {

struct FakeUIData {
  bool has_display = true;
  bool open_fails = false;
  bool window_open = false;
  bool focus = true;
  bool quit = false;
  bool woken = false;
  u32 width = 1280;
  u32 height = 720;
  f32 scale = 1.0f;
  bool fullscreen = true;
  bool exclusive = false;
  i32 monitor = 0;
  u32 min_width = 0;
  u32 min_height = 0;
  ui::AspectRatio ratio = ui::AspectRatio::kStretch;
  std::string title;
  std::string clipboard;
  hid::PointerMode pointer_mode = hid::PointerMode::kFree;
  bool cursor_visible = true;
  bool text_input = false;
  bool mouse_captured = false;
  int set_fullscreen_calls = 0;
  int set_size_calls = 0;
  int wake_calls = 0;
  std::vector<ui::HostLocale> locales;
  std::vector<ui::Monitor> displays;
  std::vector<std::string> boxes;
  u32 box_choice = 0;
  std::thread::id thread_of_last_call;
  std::deque<std::function<void(rex::ui::IUIEventListener&)>> script;
};

struct FakeUIState : FakeUIData {
  std::mutex mutex;
  std::condition_variable cv;
};

inline FakeUIState g_ui;

class FakeUIDriver : public rex::ui::IUIDriver {
 public:
  std::string_view GetName() const override { return "fake"; }
  Result<void> Initialize(IDriverContext&) override { return {}; }
  void Shutdown() override {}
  bool HasDisplay() const override { return g_ui.has_display; }
  Result<void> OpenWindow(const rex::ui::WindowConfig& config) override {
    if (g_ui.open_fails) {
      return Err(ErrorCategory::kPlatform, "the fake window refuses to open");
    }
    g_ui.window_open = true;
    g_ui.title = config.title;
    g_ui.fullscreen = config.fullscreen;
    g_ui.exclusive = config.fullscreen_exclusive;
    g_ui.monitor = config.monitor;
    g_ui.min_width = config.min_width;
    g_ui.min_height = config.min_height;
    g_ui.ratio = config.aspect_ratio;
    g_ui.width = config.width;
    g_ui.height = config.height;
    return {};
  }
  void CloseUIWindow() override { g_ui.window_open = false; }
  void SetTitle(std::string_view title) override {
    g_ui.thread_of_last_call = std::this_thread::get_id();
    g_ui.title = std::string(title);
  }
  void SetSize(u32 width, u32 height) override {
    g_ui.thread_of_last_call = std::this_thread::get_id();
    ++g_ui.set_size_calls;
    g_ui.width = width;
    g_ui.height = height;
  }
  void SetLimits(u32 min_width, u32 min_height, ui::AspectRatio ratio) override {
    g_ui.min_width = min_width;
    g_ui.min_height = min_height;
    g_ui.ratio = ratio;
  }
  void SetFullscreen(bool fullscreen, bool exclusive, i32 monitor, u32, u32) override {
    g_ui.thread_of_last_call = std::this_thread::get_id();
    ++g_ui.set_fullscreen_calls;
    g_ui.fullscreen = fullscreen;
    g_ui.exclusive = exclusive;
    g_ui.monitor = monitor;
  }
  void RaiseWindow() override {}
  void SetCursorShape(bool visible, rex::ui::CursorShape) override {
    g_ui.cursor_visible = visible;
  }
  void SetPointerMode(hid::PointerMode mode) override { g_ui.pointer_mode = mode; }
  void CaptureMouse(bool captured) override { g_ui.mouse_captured = captured; }
  f32 GetContentScale() const override { return g_ui.scale; }
  ui::WindowSize GetWindowSize() const override { return {g_ui.width, g_ui.height}; }
  bool HasFocus() const override { return g_ui.focus; }
  void SetTextInputActive(bool active) override { g_ui.text_input = active; }
  Result<rex::NativeWindow> GetNativeWindow() const override {
    if (!g_ui.window_open) {
      return Err<rex::NativeWindow>(ErrorCategory::kNotFound, "closed");
    }
    rex::NativeWindow window;
    window.type = rex::NativeWindow::Type::kX11;
    window.window = reinterpret_cast<void*>(0x1234);
    return window;
  }
  bool WaitEvents(rex::ui::IUIEventListener& listener, u32 timeout_ms) override {
    std::deque<std::function<void(rex::ui::IUIEventListener&)>> batch;
    {
      std::unique_lock lock(g_ui.mutex);
      if (g_ui.script.empty() && !g_ui.woken && timeout_ms > 0) {
        g_ui.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [] { return !g_ui.script.empty() || g_ui.woken; });
      }
      g_ui.woken = false;
      batch.swap(g_ui.script);
    }
    for (auto& step : batch) {
      step(listener);
    }
    return !g_ui.quit;
  }
  void WakeUp() override {
    {
      std::lock_guard lock(g_ui.mutex);
      g_ui.woken = true;
      ++g_ui.wake_calls;
    }
    g_ui.cv.notify_all();
  }
  Result<u32> ShowMessageBox(MessageType, std::string_view title, std::string_view,
                             std::span<const std::string>) override {
    g_ui.boxes.emplace_back(title);
    return g_ui.box_choice;
  }
  Result<std::vector<std::filesystem::path>> ShowFileDialog(const ui::FileDialogConfig&) override {
    return std::vector<std::filesystem::path>{};
  }
  std::vector<ui::Monitor> GetDisplays() override { return g_ui.displays; }
  std::vector<ui::HostLocale> GetPreferredLocales() override { return g_ui.locales; }
  std::string GetClipboardText() override { return g_ui.clipboard; }
  void SetClipboardText(std::string_view text) override { g_ui.clipboard = std::string(text); }
};

inline std::unique_ptr<IDriver> CreateFakeUI() {
  return std::make_unique<FakeUIDriver>();
}

class FakeUIProvider final : public IModule, public IDriverModule {
 public:
  std::string_view GetName() const override { return "fake_ui"; }
  Result<void> Connect(IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  Result<void> Init() override { return {}; }
  void Shutdown() override {}
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IDriverModule>(*this, name, version);
  }
  std::span<const DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<DriverRegistration, 1> kDrivers = {
      DriverRegistration{DriverType::kUI, "fake", {}, &CreateFakeUI}};
};

inline void Deliver(std::function<void(rex::ui::IUIEventListener&)> step) {
  {
    std::lock_guard lock(g_ui.mutex);
    g_ui.script.push_back(std::move(step));
  }
  g_ui.cv.notify_all();
}

inline void DeliverKey(hid::InputEvent::Type type, hid::VirtualKey key, bool repeat = false) {
  Deliver([=](rex::ui::IUIEventListener& listener) {
    hid::InputEvent event{.type = type, .key = key, .repeat = repeat};
    listener.OnInputEvent(event);
  });
}

inline void DeliverMouseButton(hid::InputEvent::Type type, hid::MouseButton button) {
  Deliver([=](rex::ui::IUIEventListener& listener) {
    hid::InputEvent event{.type = type, .button = button, .x = 4.0f, .y = 4.0f};
    listener.OnInputEvent(event);
  });
}

inline void DeliverWheelTurn(f32 detents) {
  Deliver([=](rex::ui::IUIEventListener& listener) {
    hid::InputEvent event{
        .type = hid::InputEvent::Type::kWheel, .x = 4.0f, .y = 4.0f, .scroll_y = detents};
    listener.OnInputEvent(event);
  });
}

inline void DeliverWheel(i32 detents) {
  DeliverWheelTurn(static_cast<f32>(detents));
}

inline void DeliverFocus(bool focused) {
  g_ui.focus = focused;
  Deliver([=](rex::ui::IUIEventListener& listener) {
    ui::UISetupEvent event(ui::UIEvent::Type::kFocus);
    listener.OnUIEvent(event);
  });
}

/** An app on the fake UI driver with the real UI, input and audio systems. */
class UIApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit UIApp(bool display = true) : ObjectApp("ui_test"), display_(display) {
    static_cast<FakeUIData&>(g_ui) = FakeUIData{};
    g_ui.has_display = display;
  }

  std::string input_drivers;
  bool without_input = false;
  std::function<void(rex::ui::IUISystem&)> configure;
  std::function<void(rex::ui::IUISystem&)> body;
  std::function<void()> on_release;

  Result<void> Start() { return Init(CLIArgList::Parse(std::vector<std::string>{})); }

  bool Pump() { return RunFrame(); }

  ui::IUIControl* GetUIControl() { return GetControlFactory().Get<ui::IUIControl>(); }

 protected:
  void OnModules(std::vector<std::string>& modules) override {
    if (without_input) {
      std::erase(modules, "rexhid");
    }
    ObjectApp::OnModules(modules);
  }

  void OnRegisterModules(std::vector<IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
    providers.push_back(&input_provider_);
  }

  Result<void> OnConfigure(SettingsRegistry& settings) override {
    (void)settings.SetText(SettingLayer::kTitle, "kernel.user_language", "0");
    (void)root.SetText(SettingLayer::kTitle, folder_.path.string());
    (void)GetUI().GetSettings().driver.SetText(SettingLayer::kTitle, display_ ? "fake" : "none");
    if (!input_drivers.empty()) {
      test_input::g_fake = {};
      (void)GetInput().GetSettings().drivers.SetText(SettingLayer::kTitle, input_drivers);
    }
    if (configure) {
      configure(GetUI());
    }
    return Ok();
  }

  void OnRelease() override {
    if (on_release) {
      on_release();
    }
  }

  Result<void> OnInit() override {
    if (body) {
      body(GetUI());
    }
    return Ok();
  }

 private:
  bool display_;
  test::TempFolder folder_{"rex_ui_"};
  FakeUIProvider provider_;
  test_input::FakeProvider input_provider_;
};

}  // namespace rex::test_ui
