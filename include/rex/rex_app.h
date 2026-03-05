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

#include <rex/ppc/image_info.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
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
};

namespace ui {
class DebugOverlayDialog;
class ConsoleDialog;
class SettingsDialog;
}  // namespace ui

/// Base class for recompiled Xbox 360 applications.
///
/// Absorbs all boilerplate: runtime setup, window creation, ImGui wiring,
/// module launch, and shutdown. Consumer projects inherit this and optionally
/// override virtual hooks for customization.
///
/// The generated main.cpp from `rexglue init` / `rexglue migrate` uses this:
/// @code
///   class MyApp : public rex::ReXApp {
///   public:
///       using rex::ReXApp::ReXApp;
///       static std::unique_ptr<rex::ui::WindowedApp> Create(
///           rex::ui::WindowedAppContext& ctx) {
///         return std::unique_ptr<MyApp>(new MyApp(ctx, "my_app",
///             PPCImageConfig));
///       }
///   };
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

  /// Called after runtime is fully initialized, before window creation.
  virtual void OnPostSetup() {}

  /// Called after ImGui drawer is created. Add custom dialogs here.
  virtual void OnCreateDialogs(ui::ImGuiDrawer* drawer) { (void)drawer; }

  /// Called before cleanup begins. Release custom resources here.
  virtual void OnShutdown() {}

  /// Called after path defaults are computed, before Runtime is constructed.
  /// Override to adjust game/user/update data paths programmatically.
  virtual void OnConfigurePaths(PathConfig& paths) { (void)paths; }

  // --- Accessors for subclass use ---
  Runtime* runtime() const { return runtime_.get(); }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }

  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }

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
};

}  // namespace rex
