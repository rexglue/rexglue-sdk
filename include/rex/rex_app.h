/**
 * @file        rex/rex_app.h
 * @brief       ReXApp - base class for recompiled windowed applications
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>

#include <rex/image_info.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>

namespace rex {

class LogCaptureSink;

/// Content path configuration, passed to OnConfigurePaths().
/// All paths start with sensible defaults derived from CLI args and cvars.
/// Subclasses may override any field before Runtime is constructed.
struct PathConfig {
  std::filesystem::path game_data_root;
  std::filesystem::path user_data_root;
  std::filesystem::path update_data_root;
  std::filesystem::path cache_root;
};

namespace ui {
class ConsoleDialog;
class SettingsDialog;
}  // namespace ui

/// Base class for recompiled Xbox 360 applications.
///
/// Absorbs all boilerplate: runtime setup, window creation, ImGui wiring,
/// module launch, and shutdown. Consumer projects inherit this and optionally
/// override virtual hooks for customization.
///
/// The generated src/{name}_app.h from `rexglue init` uses this:
/// @code
///   // src/my_app_app.h (yours to customize)
///   class MyApp : public rex::ReXApp {
///   public:
///       using rex::ReXApp::ReXApp;
///       static std::unique_ptr<rex::ui::WindowedApp> Create(
///           rex::ui::WindowedAppContext& ctx) {
///         return std::unique_ptr<MyApp>(new MyApp(ctx, "my_app",
///             PPCImageConfig));
///       }
///       // Override hooks: OnPreSetup, OnPostSetup, OnCreateDialogs, etc.
///   };
///
///   // src/main.cpp
///   #include "generated/my_app_config.h"
///   #include "generated/my_app_init.h"
///   #include "my_app_app.h"
///   REX_DEFINE_APP(my_app, MyApp::Create)
/// @endcode
class ReXApp : public ui::WindowedApp, public ui::WindowListener, public ui::WindowInputListener {
 public:
  ~ReXApp() override;

 protected:
  ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
         std::string_view usage = "[game_directory]");

  // --- Virtual hooks for customization ---

  /// Called before Runtime::Setup(). Override to modify backend config.
  virtual void OnPreSetup(RuntimeConfig& config) {}

  /// Called before Runtime::LoadXexImage(). Override to modify xex image.
  virtual void OnLoadXexImage(std::string& xex_image) {}

  /// Called after runtime is fully initialized, before window creation.
  virtual void OnPostSetup() {}

  /// Called after ImGui drawer is created. Add custom dialogs here.
  virtual void OnCreateDialogs(ui::ImGuiDrawer* drawer) { (void)drawer; }

  /// Called before cleanup begins. Release custom resources here.
  virtual void OnShutdown() {}

  /// Called after path defaults are computed, before Runtime is constructed.
  /// Override to adjust game/user/update data paths programmatically.
  virtual void OnConfigurePaths(PathConfig& paths) { (void)paths; }

  /// Called after Runtime::LoadXexImage() succeeds. The XEX is loaded and
  /// mapped into guest memory but the module has not launched.
  /// Use this for data patches on the loaded image.
  virtual void OnPostLoadXexImage() {}

  /// Called immediately before the main guest thread is created.
  /// Everything is set up -- last chance to patch guest memory/code.
  virtual void OnPreLaunchModule() {}

  /// Called after the main guest thread is created but before it starts
  /// executing. The thread is suspended -- attach debuggers/monitors here.
  virtual void OnPostLaunchModule(system::XThread* thread) { (void)thread; }

  /// Called when the main guest thread exits. The runtime is still alive.
  /// Use for cleanup that depends on runtime resources.
  virtual void OnGuestThreadExit(system::XThread* thread) { (void)thread; }

  // --- Accessors for subclass use ---
  Runtime* runtime() const { return runtime_.get(); }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }

  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }
  const std::filesystem::path& cache_root() const { return cache_root_; }

  /// Set a callback that provides guest frame stats to the debug overlay.
  void SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider);

 private:
  // WindowedApp overrides
  bool OnInitialize() override;
  void OnDestroy() override;

  // WindowListener overrides
  void OnClosing(ui::UIEvent& e) override;

  // WindowInputListener overrides
  void OnKeyDown(ui::KeyEvent& e) override;

  PPCImageInfo ppc_info_;
  std::filesystem::path game_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path cache_root_;
  std::unique_ptr<Runtime> runtime_;
  std::unique_ptr<ui::Window> window_;
  std::thread module_thread_;
  std::atomic<bool> shutting_down_{false};
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;

  // Built-in overlays
  std::shared_ptr<LogCaptureSink> log_sink_;
  std::unique_ptr<ui::DebugOverlayDialog> debug_overlay_;
  std::unique_ptr<ui::ConsoleDialog> console_overlay_;
  std::unique_ptr<ui::SettingsDialog> settings_overlay_;
  ui::DebugOverlayDialog::FrameStatsProvider frame_stats_provider_;
  std::filesystem::path config_path_;
};

}  // namespace rex
