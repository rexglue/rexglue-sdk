/**
 * @file        core/app.cpp
 * @brief       rex::App and the shutdown request functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/app.h>

#include <atomic>
#include <csignal>
#include <format>
#include <mutex>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <rex/platform/thread.h>

#include "core/app_state.h"
#include "core/module_loader.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <pthread.h>

#include <memory>
#include <thread>
#endif

namespace rex {
namespace {

std::atomic<bool> g_requested{false};
std::atomic<i32> g_exit_code{0};
std::mutex g_state_mutex;
std::move_only_function<void()> g_wake;
std::optional<std::vector<std::string>> g_restart;
detail::RelaunchFunction g_test_relaunch = nullptr;
bool g_test_can_relaunch = true;

#if REX_PLATFORM_WIN32
std::atomic<bool> g_teardown_done{false};

BOOL WINAPI ConsoleHandler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      RequestShutdown(0);
      return TRUE;
    case CTRL_CLOSE_EVENT:
      RequestShutdown(0);
      g_teardown_done.wait(false);
      return TRUE;
    default:
      return FALSE;
  }
}

void InstallSignalHandlers() {
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
}

void RemoveSignalHandlers() {
  SetConsoleCtrlHandler(ConsoleHandler, FALSE);
}

void BeginTimerResolution() {
  timeBeginPeriod(1);
}

void EndTimerResolution() {
  timeEndPeriod(1);
}

void ResetTeardown() {
  g_teardown_done = false;
}

void FinishTeardown() {
  g_teardown_done = true;
  g_teardown_done.notify_all();
}
#else
struct sigaction g_old_int{};
struct sigaction g_old_term{};
pthread_t g_signal_thread_id{};
std::shared_ptr<std::atomic<bool>> g_signal_stop;

sigset_t ShutdownSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}

void ForwardSignal(int received) {
  pthread_kill(g_signal_thread_id, received);
}

void InstallSignalHandlers() {
  const sigset_t set = ShutdownSignals();
  sigset_t old_mask;
  pthread_sigmask(SIG_BLOCK, &set, &old_mask);
  auto stop = std::make_shared<std::atomic<bool>>(false);
  std::thread waiter([set, stop] {
    for (;;) {
      int received = 0;
      if (sigwait(&set, &received) != 0 || stop->load()) {
        return;
      }
      RequestShutdown(0);
    }
  });
  g_signal_thread_id = waiter.native_handle();
  g_signal_stop = std::move(stop);
  waiter.detach();
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  struct sigaction action{};
  action.sa_handler = ForwardSignal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, &g_old_int);
  sigaction(SIGTERM, &action, &g_old_term);
}

void RemoveSignalHandlers() {
  sigaction(SIGINT, &g_old_int, nullptr);
  sigaction(SIGTERM, &g_old_term, nullptr);
  if (g_signal_stop) {
    g_signal_stop->store(true);
    pthread_kill(g_signal_thread_id, SIGTERM);
    g_signal_stop.reset();
  }
}

void BeginTimerResolution() {}

void EndTimerResolution() {}

void ResetTeardown() {}

void FinishTeardown() {}
#endif

std::string JoinOptions(const std::vector<std::string>& options) {
  std::string joined;
  for (const std::string& option : options) {
    joined += joined.empty() ? "'" + option + "'" : ", '" + option + "'";
  }
  return joined;
}

}  // namespace

void RequestShutdown(i32 exit_code) {
  bool expected = false;
  if (g_requested.compare_exchange_strong(expected, true)) {
    g_exit_code.store(exit_code);
  }
  std::lock_guard lock(g_state_mutex);
  if (g_wake) {
    g_wake();
  }
}

void RequestRestart(std::vector<std::string> args) {
  {
    std::lock_guard lock(g_state_mutex);
    g_restart = std::move(args);
  }
  RequestShutdown(0);
}

bool ShutdownRequested() {
  return g_requested.load();
}

namespace detail {

void ResetAppState() {
  g_requested.store(false);
  g_exit_code.store(0);
  std::lock_guard lock(g_state_mutex);
  g_wake = {};
  g_restart.reset();
  ResetTeardown();
}

void SetShutdownWake(std::move_only_function<void()> wake) {
  std::lock_guard lock(g_state_mutex);
  g_wake = std::move(wake);
}

std::optional<i32> RequestedExitCode() {
  if (!g_requested.load()) {
    return std::nullopt;
  }
  return g_exit_code.load();
}

std::optional<std::vector<std::string>> TakeRestartArgs() {
  std::lock_guard lock(g_state_mutex);
  auto args = std::move(g_restart);
  g_restart.reset();
  return args;
}

void SetRelaunchForTest(RelaunchFunction relaunch, bool can_relaunch) {
  g_test_relaunch = relaunch;
  g_test_can_relaunch = can_relaunch;
}

Result<void> RelaunchProcess(std::span<const std::string> args) {
  return g_test_relaunch ? g_test_relaunch(args) : platform::Relaunch(args);
}

bool CanRelaunchProcess() {
  return g_test_relaunch ? g_test_can_relaunch : platform::CanRelaunch();
}

}  // namespace detail

App::~App() = default;

Result<void> App::OnConfigure(Config&) {
  return Ok();
}

Result<void> App::OnInit(Config&, API&) {
  return Ok();
}

i32 App::OnRun(API&) {
  return 0;
}

void App::OnShutdown() {}

void App::Stop() {}

void App::Release() {}

void App::ReportError(std::string_view message) {
  REXLOG_ERROR("{}", message);
}

const CLIArgList& App::args() const {
  return args_;
}

API& App::api() {
  return *api_;
}

Config& App::config() {
  return config_;
}

LogConfig& App::log() {
  return log_;
}

Result<void> App::Fail(std::string_view step, Error error) {
  failed_ = true;
  ReportError(std::format("{} failed: {}", step, error.what()));
  return std::unexpected(std::move(error));
}

void App::StartLogging() {
  auto* cvars = api_->Get<cvar::ICvarRegistry>();
  std::string level = cvars->Get("log.level").value_or("info");
  if (cvars->Get("log.verbose").value_or("false") == "true" && level == "info") {
    level = "trace";
  }
  LogConfig built = BuildLogConfig(level, ParseCategoryLevelsFromConfig(config_.paths.config_file));
  log_.default_level = built.default_level;
  for (auto& [category, category_level] : built.category_levels) {
    log_.category_levels.try_emplace(category, category_level);
  }
  ApplyLogCvarOverrides(log_);
  InitLogging(log_);
  RegisterLogLevelCallback();
  logging_ = true;
}

Result<void> App::Init(const CLIArgList& args) {
  if (stage_ != Stage::kNone) {
    return Err<void>(ErrorCategory::Runtime, "App::Init called twice");
  }
  args_ = args;
  stage_ = Stage::kConfigured;
  detail::ResetAppState();
  InitLoggingEarly();
  InstallSignalHandlers();

  if (auto configured = OnConfigure(config_); !configured) {
    return Fail("OnConfigure", configured.error());
  }
  if (log_.app_name.empty()) {
    log_.app_name = filesystem::GetExecutablePath().stem().string();
  }
  if (log_.log_dir.empty()) {
    log_.log_dir = filesystem::GetExecutableFolder() / "logs";
  }
  if (auto unknown = ParseCommandLine(config_, args_); !unknown.empty()) {
    return Fail("command line",
                Error(ErrorCategory::Config, "unknown option " + JoinOptions(unknown)));
  }
  if (!config_.single_instance.empty()) {
    auto lock = platform::InstanceLock::Acquire(config_.single_instance);
    if (!lock) {
      return Fail("instance lock", lock.error());
    }
    lock_.emplace(std::move(*lock));
  }
  if (!config_.paths.config_file.empty()) {
    if (auto loaded = LoadConfigFile(config_, config_.paths.config_file); !loaded) {
      return Fail("config file", loaded.error());
    }
  }
  cvar::ApplyEnvironment(config_);

  auto& loader = detail::ModuleLoader::Instance();
  auto api = loader.Connect(config_);
  if (!api) {
    return Fail("module connect", api.error());
  }
  api_ = *api;
  stage_ = Stage::kConnected;

  StartLogging();
  if (api_->Get<cvar::ICvarRegistry>()->Get("perf.profiler").value_or("false") == "true") {
    platform::Profiler::Startup();
  }
  BeginTimerResolution();
  timer_resolution_ = true;

  if (auto initialized = OnInit(loader.mutable_config(), *api_); !initialized) {
    return Fail("OnInit", initialized.error());
  }
  if (auto modules = loader.InitModules(); !modules) {
    return Fail("module init", modules.error());
  }
  stage_ = Stage::kInitialized;
  return Ok();
}

void App::Run() {
  if (stage_ == Stage::kInitialized) {
    run_code_ = OnRun(*api_);
  }
}

i32 App::Shutdown() {
  if (stage_ == Stage::kShutdown) {
    return final_code_;
  }
  if (stage_ == Stage::kNone) {
    stage_ = Stage::kShutdown;
    return final_code_;
  }
  Stop();
  if (stage_ == Stage::kInitialized) {
    OnShutdown();
  }
  Release();
  if (stage_ == Stage::kConnected || stage_ == Stage::kInitialized) {
    rex::Shutdown();
  }
  api_ = nullptr;
  platform::Profiler::Shutdown();
  if (timer_resolution_) {
    EndTimerResolution();
  }
  platform::detail::ShutdownTimerQueue();
  lock_.reset();

  if (auto requested = detail::RequestedExitCode()) {
    final_code_ = *requested;
  } else if (failed_) {
    final_code_ = 1;
  } else {
    final_code_ = run_code_.value_or(0);
  }
  if (auto restart = detail::TakeRestartArgs(); restart && detail::CanRelaunchProcess()) {
    std::vector<std::string> relaunch_args = *restart;
    if (relaunch_args.empty()) {
      relaunch_args.assign(args_.All().begin(), args_.All().end());
    }
    if (auto relaunched = detail::RelaunchProcess(relaunch_args); !relaunched) {
      REXLOG_ERROR("relaunch failed: {}", relaunched.error().what());
    }
  }
  if (logging_) {
    ShutdownLogging();
  }
  RemoveSignalHandlers();
  detail::SetShutdownWake({});
  stage_ = Stage::kShutdown;
  FinishTeardown();
  return final_code_;
}

}  // namespace rex
