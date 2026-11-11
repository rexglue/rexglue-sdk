/**
 * @file        app/title_app.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string_view>

#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/ppc/image.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_exit.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::app {

/**
 * An app that runs a title. REX_TITLE_APP calls Init, LaunchTitle, Run and Shutdown in order. A
 * project with an installer or a launcher uses REX_TITLE_APP_MAIN, calling RunFrame until it is
 * ready to launch. LaunchTitle and UnloadTitle may run any number of times, each launch from a
 * fresh mapping. When the title exits on its own the app asks the player to restart it or exit, or
 * shuts down when headless. Every step and hook runs on the UI thread that called Init.
 */
class REX_APP_API TitleApp : public App {
 public:
  /**
   * @param image The recompiled image the codegen header declares.
   */
  TitleApp(std::string_view app_name, const ppc::PPCImage& image);
  ~TitleApp() override;

  /**
   * The launch phase. Runs TitleRuntime::Load, OnTitleLaunch, then TitleRuntime::Launch. Call on
   * the UI thread after Init. Returns Ok and does nothing once shutdown is requested.
   *
   * @return Ok, or an error, which the app shows to the player.
   */
  [[nodiscard]] Result<void> LaunchTitle();

  /** The unload phase runs TitleRuntime::Unload and returns to running. Call on the UI thread. */
  void UnloadTitle();

  /**
   * Returns the title runtime, loaded or not.
   */
  rex::kernel::TitleRuntime& GetTitleRuntime();

 protected:
  /**
   * Requires the Xenos graphics family, audio, input and UI.
   */
  AppSysRequirements GetRequirements() const override;

  /**
   * Runs after TitleRuntime::Load and before the guest starts.
   *
   * @return Ok, or an error that stops the launch.
   */
  [[nodiscard]] virtual Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title);

  /** The title exited on its own. The default asks the player to restart or exit. */
  virtual void OnTitleExit(rex::kernel::TitleExitReason reason);

  /** Runs the title until it exits or shutdown is requested. */
  void OnRun() override;

  /**
   * Reports an error through the host.
   *
   * @param message The error text.
   */
  void ReportError(std::string_view message) override;

  /**
   * Returns the options the crash reporter starts with.
   */
  platform::CrashReporterOptions GetCrashOptions() override;

  void OnStop() final;

  void OnRelease() override;

  void RestartTitle();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::app

/**
 * Defines main, and wWinMain on Windows, for a title app. An exception that escapes is printed to
 * stderr and the exit code is 1.
 *
 * @param AppClass A class deriving from rex::app::TitleApp whose default constructor passes the app
 * name and the kPPCImageConfig that the codegen init header declares to TitleApp.
 *
 * @code
 * #include "generated/default/foo_init.h"
 *
 * class FooApp : public rex::app::TitleApp {
 *  public:
 *   FooApp() : TitleApp("foo", kPPCImageConfig) {}
 * };
 * REX_TITLE_APP(FooApp)
 * @endcode
 */
#define REX_TITLE_APP(AppClass)                             \
  static int ReXTitleEntry(const ::rex::CLIArgList& args) { \
    AppClass rex_app;                                       \
    if (rex_app.Init(args) && rex_app.LaunchTitle()) {      \
      rex_app.Run();                                        \
    }                                                       \
    return rex_app.Shutdown();                              \
  }                                                         \
  REX_DETAIL_ENTRY_POINTS(ReXTitleEntry)

/**
 * Defines the entry point for a title app that calls its own steps, such as one with an installer.
 *
 * @param AppClass A class deriving from rex::app::TitleApp with a default constructor.
 * @param Function A function taking AppClass& and const rex::CLIArgList&, returning int.
 *
 * @code
 * int GameMain(Game& game, const rex::CLIArgList& args) {
 *   if (game.Init(args)) {
 *     while (game.NeedsInstall() && game.RunFrame()) {
 *     }
 *     if (game.LaunchTitle()) {
 *       game.Run();
 *     }
 *   }
 *   return game.Shutdown();
 * }
 * REX_TITLE_APP_MAIN(Game, GameMain)
 * @endcode
 */
#define REX_TITLE_APP_MAIN(AppClass, Function)              \
  static int ReXTitleEntry(const ::rex::CLIArgList& args) { \
    AppClass rex_app;                                       \
    return Function(rex_app, args);                         \
  }                                                         \
  REX_DETAIL_ENTRY_POINTS(ReXTitleEntry)
