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
#include <rex/gpu.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/runtime/config.h>
#include <rex/types.h>
#include <rex/ui.h>
#include <rex/ui/null_user_interface.h>
#include <rex/ui/overlay.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/user_interface.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class TitleApp;
struct Style;

/** Setup access passed to TitleApp::OnCreateWindow. Valid only while that hook runs. */
class REX_RUNTIME_API TitleUI {
 public:
  /**
   * Returns the ImGui font atlas before it is built.
   *
   * @return The atlas the ui module builds after OnCreateWindow returns.
   */
  ImFontAtlas& Fonts();

  /**
   * Returns the ImGui style.
   *
   * @return The style every overlay draws with.
   */
  ImGuiStyle& GetImGuiStyle();

  /**
   * Returns the SDK style.
   *
   * @return The style the SDK overlays read.
   */
  Style& GetStyle();

  /**
   * Returns the window being created.
   *
   * @return The open window.
   */
  IWindow& GetWindow();

  /**
   * Returns the user interface that owns the overlays.
   *
   * @return The interface the app adds its overlays to.
   */
  IUserInterface& Overlays();

  /**
   * Sets the frame time source the debug overlay reads, for apps that present the title
   * themselves.
   *
   * @param provider Function the debug overlay calls once per frame.
   */
  void SetFrameStats(FrameStatsProvider provider);

  /**
   * Gives the title window an overlay drawer when no GPU backend module is loaded.
   *
   * @param drawer The drawer the UI tier draws through.
   */
  void SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer);

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
class REX_RUNTIME_API TitleApp : public App, public gpu::IUIThread {
 public:
  /** Creates the app. Nothing starts until Init. */
  TitleApp();
  ~TitleApp() override;

  /**
   * Runs work on the UI thread through the user interface.
   *
   * @param work The work to run.
   */
  void RunSynchronous(std::move_only_function<void()> work) override;

  /**
   * Configures and initializes the app for a recompiled image. Starts the module list as
   * {"rex-apu-sdl3", "rex-hid-sdl3", "rex-gpu-xenos", "rex-ui-sdl3"}, then runs App::Init, which
   * puts rex-filesystem and rex-runtime first through RequireModules. Fetches the user interface
   * afterwards and runs headless on a null provider when no module serves one.
   *
   * @param args The process command line.
   * @param image The recompiled image descriptor codegen generates.
   * @return Ok when every module is initialized.
   */
  [[nodiscard]] Result<void> Init(const CLIArgList& args, const PPCImageInfo& image);

  /**
   * Creates the title window. Opens the window from Config::ui through the user interface, hands
   * the GPU backend its presentation, creates the overlay drawer, calls OnCreateWindow, then lets
   * the ui module build the fonts. Opens no window and skips OnCreateWindow on the headless
   * provider. Returns Ok without doing anything once shutdown has been requested.
   *
   * @return Ok when the window is open, or when shutdown has been requested.
   */
  [[nodiscard]] Result<void> CreateTitleWindow();

  /**
   * Runs 1 loop iteration. Processes window events, runs posted functions and draws the
   * overlays. Lets the app show its own UI before LaunchTitle.
   *
   * @return False once shutdown has been requested or the UI loop quit.
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
   * Runs while the window is created, before the font atlas is built. Does not run on the
   * headless provider.
   *
   * @param ui Setup access to fonts, style, window, overlays and frame stats.
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
   * Restart and Exit, and requests shutdown on the headless provider.
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
   * Returns the crash reporter options, with a dialog unless the video driver is dummy.
   *
   * @return The app name and dialog choice of the report.
   */
  platform::CrashReporterOptions CrashOptions() override;

  /**
   * Cancels every pending prompt, stops the title and every guest thread, then closes the
   * window.
   */
  void Stop() override;

  /** Clears the invoke target and shuts down the runtime. */
  void Release() override;

  /**
   * Puts rex-filesystem and then rex-runtime first in the module list when the consumer did not
   * list them, so the mount table is served before rex-runtime connects.
   *
   * @param config The configuration the modules load from.
   */
  void RequireModules(Config& config) override;

  /**
   * Returns the runtime.
   *
   * @return The runtime, or nullptr before LaunchTitle and after Release.
   */
  Runtime* GetRuntime();

  /**
   * Returns the title window.
   *
   * @return The window. Valid from OnCreateWindow until Stop closes it. Never call it on the
   *         headless provider, which opens no window.
   */
  IWindow& GetWindow();

  /**
   * Returns the user interface.
   *
   * @return The interface a module serves, or the headless fallback. Valid from Init until
   *         Release.
   */
  IUserInterface& UI();

 private:
  friend class TitleUI;
  friend struct TitleAppProbe;

  bool OnWindowEvent(const UIEvent& event);
  Result<void> LaunchFailure(std::string_view message);

  PPCImageInfo image_{};
  gpu::IBackend* gpu_ = nullptr;
  IUserInterface* ui_ = nullptr;
  NullUserInterface null_ui_;
  std::unique_ptr<Runtime> runtime_;
  std::jthread title_monitor_;
  bool com_initialized_ = false;
  bool events_ready_ = false;
};

/**
 * Queues a function to run on the UI thread during the next frame. Callable from any thread. The
 * target is the user interface of the live title app, set by Init and cleared by Release, the
 * only process wide state the app keeps.
 *
 * @param fn Function to run on the UI thread.
 * @return False when no title app is live or the UI thread runs no more functions, in which case
 *         fn is discarded.
 */
REX_RUNTIME_API bool InvokeFunction(std::move_only_function<void()> fn);

}  // namespace rex::ui

namespace rex::detail {

/**
 * Returns the process command line on Windows, where wWinMain receives no argv.
 *
 * @return The parsed arguments without the program name.
 */
REX_RUNTIME_API CLIArgList WindowsCommandLine();

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
