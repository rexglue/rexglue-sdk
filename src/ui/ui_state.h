/**
 * @file        ui/ui_state.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/input_keys.h>
#include <rex/logging/logger.h>
#include <rex/title.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/utility.h>
#include <rex/settings_registry.h>
#include <rex/types.h>
#include <rex/ui/style.h>
#include "ui/ui_system.h"

#include "ui/imgui_host.h"
#include "ui/overlay/achievement_toast.h"
#include "ui/overlay/console/model.h"
#include "ui/overlay/console_command_table.h"
#include "ui/overlay/console_host.h"
#include "ui/overlay/bind_table.h"
#include "ui/overlay/overlay_context.h"
#include "ui/overlay/overlay_registry.h"

namespace rex::apu {
class IAudioSystem;
}  // namespace rex::apu

namespace rex::gpu {
class IGraphicsSystem;
}  // namespace rex::gpu

namespace rex::hid {
class IInputSystem;
}  // namespace rex::hid

namespace rex::ui {

struct UISystem::State {
  explicit State(UISystem& owner_system);
  ~State();

  struct Dispatcher final : rex::ui::IUIEventListener {
    explicit Dispatcher(State& owner_state) : state(owner_state) {}
    void OnUIEvent(const ui::UIEvent& event) override;
    void OnInputEvent(InputEvent& event) override;
    State& state;
  };

  UISystem& owner;
  Dispatcher dispatcher{*this};
  rex::ui::IUIDriver* driver = nullptr;
  rex::hid::IInputSystem* input = nullptr;
  rex::apu::IAudioSystem* audio = nullptr;
  rex::gpu::IGraphicsSystem* graphics = nullptr;
  MoveOnlyFunction<void(u32, u32)> resize_handler;
  std::thread::id ui_thread;

  std::atomic<bool> started{false};
  std::atomic<bool> headless{true};
  std::atomic<bool> window_open{false};
  std::atomic<bool> focused{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> stopped{false};
  std::atomic<bool> quit{false};
  std::atomic<u32> dialog_count{0};
  std::atomic<u32> overlay_visible_count{0};

  std::mutex post_mutex;
  std::condition_variable post_wake;
  std::deque<MoveOnlyFunction<void()>> posts;
  u32 sync_calls = 0;
  std::condition_variable sync_idle;

  std::mutex listener_mutex;
  std::vector<rex::ui::IUIEventListener*> listeners;
  std::mutex delivery_mutex;

  std::mutex frame_mutex;

  std::unique_ptr<ui::ImGuiHost> imgui;
  ui::Style style;
  ui::Style applied_style;
  MoveOnlyFunction<void()> close_handler;
  ISettingsRegistry* registry = nullptr;
  ILogger* logger = nullptr;
  ui::BindTable binds;
  std::unique_ptr<ui::OverlayRegistry> overlays;
  std::unique_ptr<ui::ModuleOverlayContext> overlay_context;
  std::unique_ptr<log::LogCaptureSink> log_capture;
  ScopeGuard log_capture_guard;
  ScopeGuard pointer_guard;
  std::unique_ptr<ui::ConsoleModel> console;
  std::unique_ptr<ui::ConsoleCommandTable> console_commands;
  std::unique_ptr<ui::ConsoleDockHost> console_host;
  ui::FrameStatsProvider frame_stats;
  TitleInfo title_info;
  IAchievementRegistry* achievements = nullptr;
  u32 prompt_count = 0;
  std::mutex toast_mutex;
  std::unique_ptr<ui::AchievementToastDialog> toast;
  ScopeGuard toast_listener;

  u32 window_width = 0;
  u32 window_height = 0;
  f32 content_scale = 1.0f;
  PointerMode applied_pointer_mode = PointerMode::kFree;
  bool cursor_hidden_by_timeout = false;
  bool block_acquired = false;
  std::chrono::steady_clock::time_point last_pointer_motion;
  std::vector<ScopeGuard> watches;

  bool IsUIThread() const { return std::this_thread::get_id() == ui_thread; }
  void ExecutePosts();
  void WakeLoop();

  void OpenWindow();
  void CloseWindow();
  void WatchWindowSettings();
  void RequestClose();
  void OnEvent(ui::UIEvent& event);
  void OnInputEvent(InputEvent& event);
  void RefreshCapture();
  bool IsCaptureActive() const;
  bool IsOverlayCapturingInput(bool keyboard) const;
  bool IsHeldByOverlay(const InputEvent& event) const;
  void ApplyPointerMode(PointerMode mode);
  void UpdateCursorTimeout();
  void SyncOverlayBlock();
  void CreateImGui();
  void CreateToast();
  void ReleaseToast();
};

}  // namespace rex::ui
