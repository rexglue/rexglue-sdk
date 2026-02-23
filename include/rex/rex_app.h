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
#include <memory>
#include <string_view>
#include <thread>

#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>

namespace rex {

/// Built-in debug overlay showing FPS counter.
/// Managed internally by ReXApp; users can add custom dialogs via OnCreateDialogs().
class DebugOverlayDialog : public ui::ImGuiDialog {
 public:
  explicit DebugOverlayDialog(ui::ImGuiDrawer* imgui_drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;
};

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
///   };
///   REX_DEFINE_APP(my_app, MyApp::Create)
/// @endcode
class ReXApp : public ui::WindowedApp, public ui::WindowListener {
 public:
  /// Factory function for the windowed app system.
  /// Subclasses don't need to override this unless they need custom construction.
  static std::unique_ptr<ui::WindowedApp> Create(ui::WindowedAppContext& ctx);

  ~ReXApp() override = default;

 protected:
  ReXApp(ui::WindowedAppContext& ctx, std::string_view name,
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

  // --- Accessors for subclass use ---
  Runtime* runtime() const { return runtime_.get(); }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }

 private:
  // WindowedApp overrides
  bool OnInitialize() override;
  void OnDestroy() override;

  // WindowListener overrides
  void OnClosing(ui::UIEvent& e) override;

  std::unique_ptr<Runtime> runtime_;
  std::unique_ptr<ui::Window> window_;
  std::thread module_thread_;
  std::atomic<bool> shutting_down_{false};
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<DebugOverlayDialog> debug_overlay_;
};

}  // namespace rex
