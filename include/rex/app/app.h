/**
 * @file        app/app.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cli.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/app_info.h>
#include <rex/module.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/types.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex {

namespace gpu {
class IGraphicsSystem;
}  // namespace gpu
namespace apu {
class IAudioSystem;
}  // namespace apu
namespace hid {
class IInputSystem;
}  // namespace hid
namespace ui {
class IUISystem;
}  // namespace ui

}  // namespace rex

namespace rex::app {

/**
 * Phase of the app lifecycle. kTitleLaunch, kTitleRunning and kTitleStop exist only in a
 * TitleApp.
 */
enum class AppPhase : u8 {
  kStart,       /**< Init. Logging, settings and paths come up and the layers apply. */
  kProviders,   /**< Modules load and connect and drivers initialize. */
  kRunning,     /**< Window, input, audio, UI and overlays work, with no title. */
  kTitleLaunch, /**< rex::kernel::TitleRuntime loads and launches and drivers attach. */
  kTitleRunning,
  kTitleStop, /**< rex::kernel::TitleRuntime stops, drivers detach, then kRunning again. */
  kShutdown,  /**< Drivers shut down, modules unload in reverse and logging goes last. */
};

/**
 * A program derives from it, overrides the hooks it needs and hands the class to REX_TITLE_APP or
 * REX_CONSOLE_APP. It owns the app.* path settings.
 */
class REX_APP_API App : public SettingOwner {
 public:
  REX_SETTING(std::string, name, "", "App name, set by the constructor", kStartupOnly, kNoSave);
  REX_SETTING(std::filesystem::path, root, "",
              "Folder the app keeps its files in, the folder of the executable when empty",
              kStartupOnly);
  REX_SETTING(std::filesystem::path, log_root, "{app.root}/logs", "Log folder", kStartupOnly);
  REX_SETTING(std::string, single_instance, "",
              "Lock name only one running copy may hold, empty for none", kStartupOnly);

  /**
   * @param name The app name, which names the config file and the log file.
   */
  explicit App(std::string_view name);
  ~App() override;

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  std::string_view GetName() const;

  AppPhase GetPhase() const;

  /**
   * Returns the positional command line arguments the framework did not consume.
   */
  std::span<const std::string> GetArgs() const;

  /** Returns the input system. Its settings register in Init and it starts with the modules. */
  rex::hid::IInputSystem& GetInput();

  rex::gpu::IGraphicsSystem& GetGraphics();

  rex::apu::IAudioSystem& GetAudio();

  rex::ui::IUISystem& GetUI();

  /**
   * Asks the app to shut down, stopping a running title first. Any thread.
   */
  void RequestShutdown();

  /**
   * Asks the app to shut down and start the process again with the same arguments, so kRestart
   * settings take effect. Any thread.
   */
  void RequestRestart();

  /**
   * The start and modules phases. Ends with OnInit.
   *
   * A shutdown or restart requested in OnConfigure or in OnAction ends startup once the
   * log file is open, before the crash reporter and before any driver module loads. OnInit does not
   * run, Init returns Ok and logs no error, and the caller goes on to Shutdown. A caller that runs
   * Init itself tells the two outcomes apart with rex::app::IsShutdownRequested.
   *
   * @return Ok after OnInit, or Ok with startup ended early because a shutdown or restart was
   *         requested, or an error, which the app logged and showed.
   */
  [[nodiscard]] Result<void> Init(const CLIArgList& args);

  /**
   * Runs one frame of the running phase, delivering window events and the work posted to the UI
   * thread.
   *
   * @return False once a shutdown was requested or the UI loop ended.
   */
  bool RunFrame();

  /** The running phase. Calls OnRun. */
  void Run();

  /**
   * The shutdown phase. Safe after a failed Init.
   *
   * @return The exit code passed to rex::app::RequestShutdown if there was one, else 1 when a
   *         step failed, else 0.
   */
  int Shutdown();

 protected:
  /**
   * Returns the systems this program needs. The app uses it to choose driver plugins and to fail
   * startup when a required system has no driver. The default requires nothing.
   */
  virtual AppSysRequirements GetRequirements() const;

  /**
   * Init, after the modules connect and before the config file, environment and command line
   * apply. Set title layer values and register the program's own setting owners here. A shutdown or
   * restart requested here ends startup before any driver module loads.
   *
   * @return Ok, or an error that fails Init.
   */
  [[nodiscard]] virtual Result<void> OnConfigure(SettingsRegistry& settings);

  /**
   * Returns the command line actions this program accepts, such as --version. The default has none.
   */
  virtual std::span<const CLIAction> GetActions() const;

  /**
   * Init, after the command line is read, once for each action given. A shutdown or restart
   * requested here ends startup once the log file is open, before any driver module loads.
   *
   * @param hit The action and the text given with it.
   * @return Ok, or an error that fails Init.
   */
  [[nodiscard]] virtual Result<void> OnAction(const CLIActionHit& hit);

  /**
   * Init, before any module loads. Edits the module libraries rex::Init loads, in order. The
   * default is rexfilesystem, rexhid, rexapu, rexui, rexgpu, rexxam, rexxbdm and rexkernel.
   *
   * @param modules The library names. The driver plugins load after them.
   */
  virtual void OnModules(std::vector<std::string>& modules);

  /**
   * Init, before modules connect. Register modules compiled into the program here.
   *
   * @param modules Where to add them. The app does not own them.
   */
  virtual void OnRegisterModules(std::vector<IModule*>& modules);

  /**
   * Init, last. The systems run and the window is open. Add fonts, styles and overlays here.
   *
   * @return Ok, or an error that fails Init.
   */
  [[nodiscard]] virtual Result<void> OnInit();

  /** The default calls RunFrame until a shutdown is requested. */
  virtual void OnRun();

  /** Shutdown, before drivers shut down. Logging and settings work. */
  virtual void OnShutdown();

  /** Reports an error from a lifecycle step. The base class logs it. */
  virtual void ReportError(std::string_view message);

  /** The base class names the app after the executable and shows no dialog. */
  virtual platform::CrashReporterOptions GetCrashOptions();

  /**
   * Returns the UI system, or null while the systems are not connected and after a failed start
   * released them.
   */
  rex::ui::IUISystem* FindUI();

  /**
   * Reports whether the program asked for no UI driver, in any layer or on the command line. The
   * answer holds after the UI system is gone.
   */
  bool IsHeadlessRequested() const;

  /**
   * Marks a failed lifecycle step so Shutdown returns 1, and reports it.
   *
   * @param step Name of the step, used in the report.
   * @return The error, for the step to return.
   */
  [[nodiscard]] Result<void> Fail(std::string_view step, Error error);

  /** Stops what a derived app type runs. Shutdown calls it first, before OnShutdown. */
  virtual void OnStop();

  /**
   * Releases what a derived app type created after Init. Runs after OnShutdown.
   */
  virtual void OnRelease();

  /** Returns the internal interfaces of the systems, for TitleRuntime::Launch. */
  IInterfaceFactory& GetControlFactory();

  /** Moves the app to a phase between kRunning and kTitleStop, for an app that runs a title. */
  void SetPhase(AppPhase phase);

 private:
  [[nodiscard]] Result<void> ReportUnknownSettings();

  struct State;
  std::unique_ptr<State> state_;
};

/**
 * Requests shutdown of the running app. Any thread. Later requests do not change the exit code.
 *
 * @param exit_code The exit code Shutdown returns.
 */
REX_APP_API void RequestShutdown(i32 exit_code);

/**
 * Requests shutdown of the running app and a new process of it once Shutdown completes. Any
 * thread.
 *
 * @param args Arguments for the new process. Empty reuses the current arguments.
 */
REX_APP_API void RequestRestart(std::vector<std::string> args = {});

/**
 * Reports whether shutdown of the running app has been requested.
 *
 * @return True once rex::app::RequestShutdown or rex::app::RequestRestart has been called.
 */
REX_APP_API bool IsShutdownRequested();

}  // namespace rex::app

/**
 * Defines the entry point for a console app. An exception that escapes is printed to stderr and the
 * exit code is 1.
 *
 * @param AppClass A class deriving from rex::app::App with a default constructor.
 *
 * @code
 * class FooTool : public rex::app::App {
 *  public:
 *   FooTool() : App("foo") {}
 * };
 * REX_CONSOLE_APP(FooTool)
 * @endcode
 */
#define REX_CONSOLE_APP(AppClass)                             \
  static int ReXConsoleEntry(const ::rex::CLIArgList& args) { \
    AppClass rex_app;                                         \
    if (rex_app.Init(args)) {                                 \
      rex_app.Run();                                          \
    }                                                         \
    return rex_app.Shutdown();                                \
  }                                                           \
  REX_DETAIL_ENTRY_POINTS(ReXConsoleEntry)

/**
 * Defines the entry point for a console app that calls its own steps.
 *
 * @param AppClass A class deriving from rex::app::App with a default constructor.
 * @param Function A function taking AppClass& and const rex::CLIArgList&, returning int.
 *
 * @code
 * int FooToolMain(FooTool& tool, const rex::CLIArgList& args) {
 *   if (tool.Init(args)) {
 *     tool.Run();
 *   }
 *   return tool.Shutdown();
 * }
 * REX_CONSOLE_APP_MAIN(FooTool, FooToolMain)
 * @endcode
 */
#define REX_CONSOLE_APP_MAIN(AppClass, Function)              \
  static int ReXConsoleEntry(const ::rex::CLIArgList& args) { \
    AppClass rex_app;                                         \
    return Function(rex_app, args);                           \
  }                                                           \
  REX_DETAIL_ENTRY_POINTS(ReXConsoleEntry)

namespace rex::app {

namespace detail {

/**
 * Prints an exception that escaped an entry point to stderr.
 *
 * @return 1, the exit code of a failed run.
 */
REX_APP_API int ReportEntryFailure(const char* what) noexcept;

/**
 * Runs the body of an entry point and turns an escaping exception into a message on stderr.
 *
 * @tparam Fn Callable that takes no arguments and returns the exit code.
 * @return What fn returns, or 1 when it throws.
 */
template <typename Fn>
int RunGuarded(Fn&& fn) noexcept {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::exception& error) {
    return ReportEntryFailure(error.what());
  } catch (...) {
    return ReportEntryFailure("unknown exception");
  }
}

#if REX_PLATFORM_WIN32
/**
 * Returns the process command line on Windows, where wWinMain receives no argv. The arguments
 * exclude the program name.
 */
REX_APP_API CLIArgList GetWindowsCommandLine();
#endif

}  // namespace detail

}  // namespace rex::app

#if REX_PLATFORM_WIN32
/**
 * Defines main and wWinMain, both calling fn with the parsed command line.
 *
 * @param fn A function taking const rex::CLIArgList& and returning int.
 *
 * @code
 * REX_DETAIL_ENTRY_POINTS(FooEntry)
 * @endcode
 */
#define REX_DETAIL_ENTRY_POINTS(fn)                                      \
  int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {                \
    return ::rex::app::detail::RunGuarded(                               \
        [] { return fn(::rex::app::detail::GetWindowsCommandLine()); }); \
  }                                                                      \
  int main(int argc, char** argv) {                                      \
    return ::rex::app::detail::RunGuarded(                               \
        [&] { return fn(::rex::CLIArgList::Parse(argc, argv)); });       \
  }
#else
#define REX_DETAIL_ENTRY_POINTS(fn)                                \
  int main(int argc, char** argv) {                                \
    return ::rex::app::detail::RunGuarded(                         \
        [&] { return fn(::rex::CLIArgList::Parse(argc, argv)); }); \
  }
#endif
