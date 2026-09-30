/**
 * @file        ui/title_app.h
 * @brief       The title app, its lifecycle steps and entry point macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/app.h>
#include <rex/cli_arg_list.h>
#include <rex/cvar/registry.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/runtime/config.h>
#include <rex/types.h>
#include <rex/ui.h>
#include <rex/ui/overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/ui_event.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex {
class Runtime;
namespace graphics {
class IGraphicsBackend;
}  // namespace graphics
}  // namespace rex

namespace rex::ui {

class AchievementNotificationDialog;
class ConsoleModel;
class ConsoleWindow;
class ImGuiDrawer;
class ImmediateDrawer;
class SDLWindowedAppContext;
class TitleApp;
class Window;
struct Style;

/** Setup access passed to TitleApp::OnCreateWindow. Valid only while that hook runs. */
class TitleUI {
 public:
  /**
   * Returns the ImGui font atlas before it is built.
   *
   * @return The atlas the drawer builds after OnCreateWindow returns.
   */
  ImFontAtlas& fonts();

  /**
   * Returns the ImGui style.
   *
   * @return The style every overlay draws with.
   */
  ImGuiStyle& style();

  /**
   * Returns the SDK theme.
   *
   * @return The theme the SDK overlays read.
   */
  Style& theme();

  /**
   * Returns the window being created.
   *
   * @return The open window.
   */
  Window& window();

  /**
   * Returns the overlay registry.
   *
   * @return The registry the app adds its overlays to.
   */
  OverlayRegistry& overlays();

  /**
   * Sets the frame time source the debug overlay reads, for apps that present the title
   * themselves.
   *
   * @param provider Function the debug overlay calls once per frame.
   */
  void SetFrameStats(DebugOverlayDialog::FrameStatsProvider provider);

  /**
   * Replaces the presenter drawer, for apps that present the title themselves.
   *
   * @param drawer Drawer the overlays draw through.
   */
  void SetDrawer(std::unique_ptr<ImmediateDrawer> drawer);

 private:
  friend class TitleApp;
  explicit TitleUI(TitleApp& app) : app_(app) {}
  TitleApp& app_;
};

/**
 * A title app. The program calls Init, CreateTitleWindow, LaunchTitle, Run and Shutdown in
 * that order, and may call RunFrame between CreateTitleWindow and LaunchTitle. Every step and
 * every hook runs on the UI thread, the thread that called Init.
 */
class TitleApp : public App {
 public:
  /** Creates the app. Nothing starts until Init. */
  TitleApp();
  ~TitleApp() override;

  /**
   * Configures and initializes the app for a recompiled image. Starts the module list as
   * {"rexgpu-xenos"}, then runs App::Init, which puts rexfilesystem and rexruntime first through
   * RequireModules.
   *
   * @param args The process command line.
   * @param image The recompiled image descriptor codegen generates.
   * @return Ok when every module is initialized.
   */
  [[nodiscard]] Result<void> Init(const CLIArgList& args, const PPCImageInfo& image);

  /**
   * Creates the title window. Opens the SDL context and the window from Config::ui, creates the
   * drawers and the overlay registry, calls OnCreateWindow, then builds the fonts. Returns Ok
   * without doing anything once shutdown has been requested.
   *
   * @return Ok when the window is open, or when shutdown has been requested.
   */
  [[nodiscard]] Result<void> CreateTitleWindow();

  /**
   * Runs 1 loop iteration. Processes window events, runs posted functions and draws the
   * overlays. Lets the app show its own UI before LaunchTitle.
   *
   * @return False once shutdown has been requested.
   */
  bool RunFrame();

  /**
   * Launches the title. Builds the runtime from the current configuration, loads the image,
   * calls OnTitleLaunch, then starts the title's main thread. Returns Ok without doing anything
   * once shutdown has been requested.
   *
   * @return Ok when the guest thread is running, or when shutdown has been requested.
   */
  [[nodiscard]] Result<void> LaunchTitle();

  /** Runs frames until shutdown is requested. */
  void Run() override;

 protected:
  /**
   * Runs while the window is created, before the font atlas is built.
   *
   * @param ui Setup access to fonts, style, theme, window, overlays, drawer and frame stats.
   */
  virtual void OnCreateWindow(TitleUI& ui);

  /**
   * Receives every window and input event before the SDK's own listeners, from the end of
   * CreateTitleWindow until Release. UIEvent::Type::Closing arrives when Stop closes the window
   * after the title stops, where the app destroys the dialogs it owns.
   *
   * @param event The event.
   * @return True consumes the event. For UIEvent::Type::Close, true keeps the window open.
   */
  virtual bool OnEvent(const UIEvent& event);

  /**
   * Runs after the runtime is set up and the image is loaded, before the title's main thread
   * starts.
   *
   * @param runtime The runtime about to start the title.
   * @return Ok to start the title. An error makes LaunchTitle return it before any guest thread
   *         runs.
   */
  virtual Result<void> OnTitleLaunch(Runtime& runtime);

  /**
   * Runs when the title exits while the app keeps running. The default shows a prompt with
   * Restart and Exit.
   *
   * @param reason Why the title exited.
   */
  virtual void OnTitleExit(runtime::TitleExitReason reason);

  /**
   * Logs the error and shows it in a message box.
   *
   * @param message The error text.
   */
  void ReportError(std::string_view message) override;

  /**
   * Destroys every dialog the drawer owns, stops the title and every guest thread, then closes the
   * window.
   */
  void Stop() override;

  /** Destroys the window and shuts down the runtime. */
  void Release() override;

  /**
   * Puts rexfilesystem and then rexruntime first in the module list when the consumer did not
   * list them, so the mount table is served before rexruntime connects.
   *
   * @param config The configuration the modules load from.
   */
  void RequireModules(Config& config) override;

  /**
   * Returns the runtime.
   *
   * @return The runtime, or nullptr before LaunchTitle and after Release.
   */
  Runtime* runtime();

  /**
   * Returns the title window.
   *
   * @return The window. Valid from OnCreateWindow until Release destroys it.
   */
  Window& window();

  /**
   * Returns the ImGui drawer.
   *
   * @return The drawer. Valid from OnCreateWindow until Release destroys it.
   */
  ImGuiDrawer& drawer();

  /**
   * Returns the overlay registry, for overlays added after the window exists.
   *
   * @return The registry. Valid from OnCreateWindow until Release destroys it.
   */
  OverlayRegistry& overlays();

 private:
  friend class TitleUI;
  class Listener;
  class BindListener;
  class ConsoleDockHost;

  Result<void> LaunchFailure(std::string_view message);
  bool CaptureActive() const;

  PPCImageInfo image_{};
  std::unique_ptr<SDLWindowedAppContext> context_;
  graphics::IGraphicsBackend* graphics_ = nullptr;
  std::unique_ptr<Window> window_;
  std::unique_ptr<Listener> listener_;
  std::unique_ptr<BindListener> bind_listener_;
  std::unique_ptr<ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ImGuiDrawer> drawer_;
  std::unique_ptr<log::CaptureSink> log_capture_;
  ILogger* logger_ = nullptr;
  DebugOverlayDialog::FrameStatsProvider frame_stats_;
  std::unique_ptr<Runtime> runtime_;
  std::unique_ptr<ConsoleModel> console_;
  std::unique_ptr<ConsoleDockHost> console_host_;
  std::unique_ptr<ConsoleWindow> console_window_;
  std::unique_ptr<OverlayContext> overlay_context_;
  std::unique_ptr<OverlayRegistry> overlays_;
  std::vector<cvar::CallbackHandle> window_callbacks_;
  std::shared_ptr<AchievementNotificationDialog> toast_;
  u64 toast_listener_ = 0;
  std::jthread title_monitor_;
  bool com_initialized_ = false;
  bool events_ready_ = false;
};

/**
 * Queues a function to run on the UI thread during the next frame. Callable from any thread.
 *
 * @param fn Function to run on the UI thread.
 * @return False when no title app has a window, in which case fn is discarded.
 */
bool InvokeFunction(std::move_only_function<void()> fn);

}  // namespace rex::ui

namespace rex::detail {

/**
 * Returns the process command line on Windows, where wWinMain receives no argv.
 *
 * @return The parsed arguments without the program name.
 */
CLIArgList WindowsCommandLine();

}  // namespace rex::detail

#if REX_PLATFORM_WIN32
/**
 * Defines main and wWinMain, both calling fn with the parsed command line. Used by the guest
 * entry point macros.
 *
 * @param fn A function taking const rex::CLIArgList& and returning int.
 *
 * @code
 * REX_DETAIL_ENTRY_POINTS(FooEntry)
 * @endcode
 */
#define REX_DETAIL_ENTRY_POINTS(fn)                       \
  int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { \
    return fn(::rex::detail::WindowsCommandLine());       \
  }                                                       \
  int main(int argc, char** argv) {                       \
    return fn(::rex::CLIArgList::Parse(argc, argv));      \
  }
#else
#define REX_DETAIL_ENTRY_POINTS(fn)                  \
  int main(int argc, char** argv) {                  \
    return fn(::rex::CLIArgList::Parse(argc, argv)); \
  }
#endif

/**
 * Defines main, and wWinMain on Windows, for a title app. The entry point parses the command
 * line, calls Init with the PPCImageConfig the codegen init header declares, then
 * CreateTitleWindow, LaunchTitle and Run on a default constructed Class while each step
 * succeeds, and returns the Shutdown exit code.
 *
 * @param Class A default constructible class derived from rex::ui::TitleApp.
 *
 * @code
 * #include "generated/default/foo_init.h"
 *
 * class FooApp : public rex::ui::TitleApp {};
 * REX_TITLE_APP(FooApp)
 * @endcode
 */
#define REX_TITLE_APP(Class)                                                 \
  static int RexTitleEntry(const ::rex::CLIArgList& args) {                  \
    Class rex_app;                                                           \
    if (rex_app.Init(args, PPCImageConfig) && rex_app.CreateTitleWindow() && \
        rex_app.LaunchTitle()) {                                             \
      rex_app.Run();                                                         \
    }                                                                        \
    return rex_app.Shutdown();                                               \
  }                                                                          \
  REX_DETAIL_ENTRY_POINTS(RexTitleEntry)

/**
 * Defines main, and wWinMain on Windows, for a title app that calls its own lifecycle steps.
 * The entry point parses the command line, default constructs Class, and returns what
 * Function returns.
 *
 * @param Class A default constructible class derived from rex::ui::TitleApp.
 * @param Function A function taking Class& and const rex::CLIArgList&, returning i32.
 *
 * @code
 * #include "generated/default/foo_init.h"
 *
 * i32 FooMain(FooApp& app, const rex::CLIArgList& args) {
 *   if (app.Init(args, PPCImageConfig) && app.CreateTitleWindow()) {
 *     while (FooSetupActive() && app.RunFrame()) {
 *     }
 *     if (app.LaunchTitle()) {
 *       app.Run();
 *     }
 *   }
 *   return app.Shutdown();
 * }
 * REX_TITLE_APP_MAIN(FooApp, FooMain)
 * @endcode
 */
#define REX_TITLE_APP_MAIN(Class, Function)                 \
  static int RexTitleEntry(const ::rex::CLIArgList& args) { \
    Class rex_app;                                          \
    return Function(rex_app, args);                         \
  }                                                         \
  REX_DETAIL_ENTRY_POINTS(RexTitleEntry)
