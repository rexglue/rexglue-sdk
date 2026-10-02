/**
 * @file        app.h
 * @brief       The app framework, lifecycle steps and shutdown requests
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/macros.h>
#include <rex/api.h>
#include <rex/cli_arg_list.h>
#include <rex/platform/crash.h>
#include <rex/platform/process.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {

/**
 * A program built on the SDK. The program calls Init, Run and Shutdown in that order. Each
 * step runs on the calling thread and calls its hook there.
 */
class REX_API App : public NonCopyable {
 public:
  virtual ~App();

  /**
   * Configures and initializes the app. Calls OnConfigure, applies the command line, the
   * single instance lock and the config file, loads and connects the modules, calls OnInit,
   * then initializes the modules. Turns on the log file before OnConfigure and names it after
   * the executable when OnConfigure leaves the name and folder empty.
   *
   * @param args The process command line.
   * @return Ok when every module is initialized. On an error the app is left for Shutdown to
   *         unwind.
   */
  [[nodiscard]] Result<void> Init(const CLIArgList& args);

  /** Runs the app. The base class calls OnRun and records the exit code it returns. */
  virtual void Run();

  /**
   * Shuts the app down. Calls Stop and OnShutdown, releases what the app created after Init,
   * shuts down and unloads the modules, and starts a new process when
   * rex::RequestRestart was called. Safe to call after any failed step.
   *
   * @return The exit code for the process. The code passed to rex::RequestShutdown when there
   *         is one, 1 when a step failed, otherwise the code OnRun returned.
   */
  i32 Shutdown();

  /**
   * Returns the command line.
   *
   * @return The arguments passed to Init.
   */
  const CLIArgList& args() const;

  /**
   * Returns the modules and core interfaces.
   *
   * @return The handle Init created. Valid from OnInit until Shutdown unloads the modules.
   */
  API& api();

 protected:
  /**
   * Returns the configuration.
   *
   * @return The configuration Init reads. Changes after Init have no effect.
   */
  Config& config();

  /**
   * Reports an error from a lifecycle step. The base class logs it. TitleApp also shows it in a
   * message box.
   *
   * @param message The error text.
   */
  virtual void ReportError(std::string_view message);

  /**
   * Returns the crash reporter options Init installs once the configuration is final. The base
   * class names the app after the executable and shows no dialog.
   *
   * @return The app name and dialog choice of the report.
   */
  virtual platform::CrashReporterOptions CrashOptions();

  /**
   * Records a failed lifecycle step so Shutdown returns 1, and reports it.
   *
   * @param step Name of the step, used in the report.
   * @param error The error.
   * @return The error, for the step to return.
   */
  Result<void> Fail(std::string_view step, Error error);

  /**
   * Sets the module list, the config slices and cvar defaults before any module loads.
   *
   * @param config The app's configuration. Init applies command line values over it
   *        afterward.
   *
   * @return Ok to continue. An error makes Init return it.
   */
  virtual Result<void> OnConfigure(Config& config);

  /**
   * Runs after every module is connected and before any module is initialized.
   *
   * @param config The configuration the modules read during their Init.
   * @param api The connected modules and the core interfaces.
   * @return Ok to continue. An error makes Init return it.
   */
  virtual Result<void> OnInit(Config& config, API& api);

  /**
   * Does the work of a console app. The base Run calls it once.
   *
   * @param api The initialized modules and the core interfaces.
   * @return The exit code. A code passed to rex::RequestShutdown takes precedence.
   */
  virtual i32 OnRun(API& api);

  /** Runs in Shutdown after Stop, while every module and everything created after Init is up. */
  virtual void OnShutdown();

  /**
   * Stops what a derived app type runs. Shutdown calls it first, before OnShutdown. TitleApp
   * stops the title and closes the window here.
   */
  virtual void Stop();

  /**
   * Adds the modules a derived app type needs to the module list. Init calls it after OnConfigure
   * and the command line, right before the modules load. TitleApp puts rexruntime first.
   *
   * @param config The configuration the modules load from.
   */
  virtual void RequireModules(Config& config);

  /**
   * Releases what a derived app type created after Init. Shutdown calls it after OnShutdown
   * and before the modules shut down. TitleApp destroys the window and shuts down the runtime
   * here.
   */
  virtual void Release();

 private:
  enum class Stage : u8 { kNone, kConfigured, kConnected, kInitialized, kShutdown };

  CLIArgList args_;
  Config config_;
  API* api_ = nullptr;
  std::optional<platform::InstanceLock> lock_;
  Stage stage_ = Stage::kNone;
  bool failed_ = false;
  bool timer_resolution_ = false;
  std::optional<i32> run_code_;
  i32 final_code_ = 0;
};

/**
 * Requests shutdown of the running app. Callable from any thread. Later requests do not
 * change the exit code.
 *
 * @param exit_code The exit code Shutdown returns.
 */
REX_API void RequestShutdown(i32 exit_code);

/**
 * Requests shutdown of the running app and a new process of it once Shutdown completes.
 * Callable from any thread.
 *
 * @param args Arguments for the new process. Empty reuses the current arguments.
 */
REX_API void RequestRestart(std::vector<std::string> args = {});

/**
 * Tests whether shutdown of the running app has been requested.
 *
 * @return True once rex::RequestShutdown or rex::RequestRestart has been called.
 */
REX_API bool ShutdownRequested();

}  // namespace rex

/**
 * Defines main for a console app. The entry point parses the command line, then calls Init,
 * Run and Shutdown on a default constructed Class and returns the Shutdown exit code.
 *
 * @param Class A default constructible class derived from rex::App.
 *
 * @code
 * class FooTool : public rex::App {
 *   i32 OnRun(rex::API& api) override;
 * };
 * REX_CONSOLE_APP(FooTool)
 * @endcode
 */
#define REX_CONSOLE_APP(Class)                                \
  int main(int argc, char** argv) {                           \
    Class rex_app;                                            \
    if (rex_app.Init(::rex::CLIArgList::Parse(argc, argv))) { \
      rex_app.Run();                                          \
    }                                                         \
    return rex_app.Shutdown();                                \
  }

/**
 * Defines main for a console app that calls its own lifecycle steps. The entry point parses
 * the command line, default constructs Class, and returns what Function returns.
 *
 * @param Class A default constructible class derived from rex::App.
 * @param Function A function taking Class& and const rex::CLIArgList&, returning i32.
 *
 * @code
 * i32 FooToolMain(FooTool& tool, const rex::CLIArgList& args) {
 *   if (tool.Init(args)) {
 *     tool.Run();
 *   }
 *   return tool.Shutdown();
 * }
 * REX_CONSOLE_APP_MAIN(FooTool, FooToolMain)
 * @endcode
 */
#define REX_CONSOLE_APP_MAIN(Class, Function)                       \
  int main(int argc, char** argv) {                                 \
    Class rex_app;                                                  \
    return Function(rex_app, ::rex::CLIArgList::Parse(argc, argv)); \
  }
