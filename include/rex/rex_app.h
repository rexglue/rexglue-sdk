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
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

#include <rex/image_info.h>
#include <rex/logging/types.h>
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
  std::filesystem::path metadata_root;
  std::filesystem::path config_path;
};

namespace ui {
class AchievementNotificationDialog;
class ConsoleDialog;
class SettingsDialog;
}  // namespace ui

/// Base class for recompiled Xbox 360 applications.
///
/// OnInitialize is a thin coordinator that runs four phases in order:
///
///   SetupEnvironment  -> paths, config, logging
///   SetupPresentation -> window, graphics presentation, ImGui drawer
///   OnFinalizePaths   -> hook for wizard-driven path resolution (sync or async)
///   ConstructRuntime  -> Runtime, guest GPU init, XEX load, rexcrt heap
///   LaunchModule      -> shader cache, PrepareModuleLaunch, background wait
///
/// Each phase is a protected virtual; consumers override selectively without
/// re-implementing the whole flow.
///
/// Subclass skeleton:
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
///       // Override hooks: OnPreSetup, OnPostSetup, OnCreateDialogs,
///       // OnConfigureFonts, OnFinalizePaths, etc.
///   };
///
///   // src/main.cpp
///   #include "generated/my_app_init.h"
///   #include "my_app_app.h"
///   REX_DEFINE_APP(my_app, MyApp::Create)
/// @endcode
class ReXApp : public ui::WindowedApp, public ui::WindowListener, public ui::WindowInputListener {
 public:
  ~ReXApp() override;

 protected:
  ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
         std::string_view usage = "");

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

  virtual void OnConfigureLogging(LogConfig& config) { (void)config; }

  /// Called after SetupPresentation returns (window and ImGui drawer are live)
  /// and before Runtime construction. Override to resolve paths from user
  /// input shown through an ImGui dialog.
  ///
  /// Return a PathConfig to continue initialization synchronously. Return
  /// std::nullopt and invoke `resume(path_config)` later (e.g. from a wizard
  /// completion handler) to continue asynchronously. `resume` must be called
  /// on the UI thread. Calling `resume` after the app has begun shutdown is
  /// a no-op.
  ///
  /// Default implementation returns `defaults` unchanged.
  virtual std::optional<PathConfig> OnFinalizePaths(const PathConfig& defaults,
                                                    std::function<void(PathConfig)> resume) {
    (void)resume;
    return defaults;
  }

  /// Called from the ImGui drawer's Initialize() after the default font is
  /// registered and before the atlas is built. Override to add additional
  /// fonts via AddFontFromMemoryTTF() or similar.
  virtual void OnConfigureFonts(ImFontAtlas* atlas) { (void)atlas; }

  /// Called from the ImGui drawer's Initialize() after the SDK defaults have
  /// been applied. `imgui_style` is the live global ImGuiStyle: patch fields,
  /// or call ImGui::StyleColorsDark(&imgui_style) first to start from a clean
  /// slate. `ui_style` carries the per-overlay colors that ImGuiStyle cannot
  /// express (achievements, toast, console, debug, settings).
  virtual void OnConfigureStyle(ImGuiStyle& imgui_style, ui::Style& ui_style) {
    (void)imgui_style;
    (void)ui_style;
  }

  /// Called after logging is initialized. Add log sinks here.
  virtual void OnPostInitLogging() {}

  /// Called after Runtime::LoadXexImage() succeeds. The XEX is loaded and
  /// mapped into guest memory but the module has not launched.
  /// Use this for data patches and recomp-specific achievement registration.
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

  /// Detached overlay mode ("bring your own renderer"). Called once from
  /// SetupPresentation when the SDK has no graphics backend
  /// (config.graphics == nullptr, typically cleared in OnPreSetup) and the app
  /// renders the guest itself. Return a unique_ptr to a ui::ImmediateDrawer
  /// subclass that creates textures and submits via your renderer. ReXApp owns
  /// the returned drawer (stored in immediate_drawer_, torn down after
  /// imgui_drawer_).
  ///
  /// Construct the drawer presenter-less. REQUIRED CONTRACT: your CreateTexture
  /// override MUST return nullptr (never crash or assert) when its GPU device
  /// is not yet available, because the SDK uploads the ImGui font atlas lazily
  /// on the first Draw and the device may only come up later (e.g. in the guest
  /// D3D device-creation hook). NOTE: ImmediateDrawer::OnEnterPresenter() /
  /// OnLeavePresenter() are NOT invoked in detached mode (the SDK never calls
  /// SetPresenter with a non-null presenter on your drawer), so perform any
  /// per-renderer GPU init lazily (on first CreateTexture/Begin), not in
  /// OnEnterPresenter. You also own present timing / vsync / letterbox in this
  /// mode.
  ///
  /// See ui::AppUIDrawContext for the per-frame draw-context handoff. Default:
  /// no overlay (SDK presenter mode; this hook is never reached).
  virtual std::unique_ptr<ui::ImmediateDrawer> OnCreateImmediateDrawer() { return nullptr; }

  // --- Window event hooks (delivered on the UI thread) ---

  /// Logical (DPI-independent) client size changed.
  virtual void OnWindowResized(uint32_t logical_width, uint32_t logical_height) {
    (void)logical_width;
    (void)logical_height;
  }

  /// Physical pixel size changed. Use this to resize swap chains.
  virtual void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) {
    (void)pixel_width;
    (void)pixel_height;
  }

  /// The user asked to close the window (close button, Alt+F4). Return false
  /// to veto and close later explicitly (window()->RequestClose()) after
  /// stopping guest threads and draining renderers. Default accepts; the
  /// window then closes and the app quits via the OnClosing path.
  virtual bool OnWindowCloseRequested() { return true; }

  virtual void OnWindowFocusChanged(bool focused) { (void)focused; }

  /// Display scale changed (window moved to a monitor with different DPI).
  /// scale is 1.0 at 96 DPI.
  virtual void OnDpiScaleChanged(float scale) { (void)scale; }

  virtual void OnWindowMinimized() {}
  virtual void OnWindowRestored() {}

  /// Creates the overlay toggled by bind_achievements. Override to replace the
  /// built-in achievement UI. Returning nullptr disables the overlay.
  virtual std::unique_ptr<ui::ImGuiDialog> CreateAchievementsOverlay();

  /// Creates the achievement notification UI. Override to replace the
  /// built-in toast renderer. Returning nullptr disables notifications.
  virtual std::unique_ptr<ui::AchievementNotificationDialog> CreateAchievementNotificationDialog();

  // --- Init phase methods (called in order from OnInitialize) ---

  /// Resolve path defaults, load config TOML, initialize logging.
  /// Populates `resolved_defaults_` with the PathConfig produced by
  /// OnConfigurePaths.
  virtual bool SetupEnvironment();

  /// Construct Runtime with the given paths, call runtime_->Setup, load the
  /// XEX image, initialize the rexcrt heap. Runs OnPostSetup at the end.
  virtual bool ConstructRuntime(const PathConfig& paths);

  /// Create the window, stand up graphics presentation, create the ImGui
  /// drawer, register overlay keybinds, run OnCreateDialogs.
  virtual bool SetupPresentation();

  /// Kick off the deferred module launch: shader storage init,
  /// PrepareModuleLaunch, main thread resume, background wait.
  virtual void LaunchModule();

  // --- Accessors for subclass use ---
  Runtime* runtime() const { return runtime_.get(); }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }
  ui::ImmediateDrawer* immediate_drawer() const { return immediate_drawer_.get(); }
  system::AchievementManager& achievements() const;

  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  const std::filesystem::path& metadata_root() const { return metadata_root_; }

  /// Set a callback that provides guest frame stats to the debug overlay.
  void SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider);

 private:
  std::function<void(PathConfig)> MakeResumeCallback();

  // Stand up the ImGui overlay stack (drawer, F3/Backtick/F4 binds, dialogs)
  // independently of how the presenter/drawer were obtained. `presenter` may be
  // null (detached mode).
  void SetupOverlays(ui::Presenter* presenter, ui::ImmediateDrawer* drawer);

  // WindowedApp overrides
  bool OnInitialize() override;
  void OnDestroy() override;

  // WindowListener overrides
  void OnClosing(ui::UIEvent& e) override;
  bool OnCloseRequested(ui::UIEvent& e) override;
  void OnResize(ui::UISetupEvent& e) override;
  void OnDpiChanged(ui::UISetupEvent& e) override;
  void OnGotFocus(ui::UISetupEvent& e) override;
  void OnLostFocus(ui::UISetupEvent& e) override;
  void OnMinimized(ui::UIEvent& e) override;
  void OnRestored(ui::UIEvent& e) override;

  // WindowInputListener overrides
  void OnKeyDown(ui::KeyEvent& e) override;

  PPCImageInfo ppc_info_;
  PathConfig resolved_defaults_;
  RuntimeConfig config_;
  std::filesystem::path game_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path metadata_root_;
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
  std::unique_ptr<ui::ImGuiDialog> achievements_overlay_;
  std::shared_ptr<ui::AchievementNotificationDialog> achievement_notification_;
  uint64_t achievement_notification_listener_ = 0;
  ui::DebugOverlayDialog::FrameStatsProvider frame_stats_provider_;
  std::filesystem::path config_path_;
};

}  // namespace rex
