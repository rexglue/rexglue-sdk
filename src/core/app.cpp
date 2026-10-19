#include <rex/app.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <format>
#include <mutex>
#include <thread>

#include <rex/platform/file.h>
#include <rex/logging/log.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <rex/platform/crash.h>
#include <rex/platform/thread.h>
#include <rex/string/utf8.h>
#include <rex/move_only_function.h>

#include "core/app_state.h"
#include "core/logger.h"
#include "core/module_loader.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#else
#include <pthread.h>

#include <memory>
#endif

namespace rex {
namespace {

constexpr u64 kRequestedBit = u64{1} << 32;
std::atomic<u64> g_request{0};
std::mutex g_state_mutex;
rex::MoveOnlyFunction<void()> g_wake;
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
std::jthread g_signal_thread;
std::shared_ptr<std::atomic<bool>> g_signal_stop;
std::atomic<bool> g_forwarding{false};
std::atomic<i32> g_forwarders{0};

sigset_t ShutdownSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}

void ForwardSignal(int received) {
  g_forwarders.fetch_add(1);
  if (g_forwarding.load()) {
    pthread_kill(g_signal_thread.native_handle(), received);
  }
  g_forwarders.fetch_sub(1);
}

void InstallSignalHandlers() {
  const sigset_t set = ShutdownSignals();
  sigset_t old_mask;
  pthread_sigmask(SIG_BLOCK, &set, &old_mask);
  auto stop = std::make_shared<std::atomic<bool>>(false);
  g_signal_thread = platform::Spawn("Shutdown Signals", [set, stop](const std::stop_token&) {
    for (;;) {
      int received = 0;
      if (sigwait(&set, &received) != 0 || stop->load()) {
        return;
      }
      RequestShutdown(0);
    }
  });
  g_signal_stop = std::move(stop);
  g_forwarding.store(true);
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  struct sigaction action{};
  action.sa_handler = ForwardSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_ONSTACK;
  sigaction(SIGINT, &action, &g_old_int);
  sigaction(SIGTERM, &action, &g_old_term);
}

void RemoveSignalHandlers() {
  sigaction(SIGINT, &g_old_int, nullptr);
  sigaction(SIGTERM, &g_old_term, nullptr);
  g_forwarding.store(false);
  while (g_forwarders.load() != 0) {
    std::this_thread::yield();
  }
  if (g_signal_stop) {
    g_signal_stop->store(true);
    // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread): SIGTERM wakes sigwait, nothing is killed
    pthread_kill(g_signal_thread.native_handle(), SIGTERM);
    g_signal_thread = {};
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
  u64 expected = 0;
  g_request.compare_exchange_strong(expected, kRequestedBit | static_cast<u32>(exit_code));
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

bool IsShutdownRequested() {
  return g_request.load() != 0;
}

namespace detail {

#if REX_PLATFORM_WIN32
CLIArgList GetWindowsCommandLine() {
  i32 count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide) {
    return CLIArgList::Parse(std::span<const std::string>{});
  }
  std::vector<std::string> args;
  for (i32 index = 1; index < count; ++index) {
    args.push_back(rex::ToUTF8(reinterpret_cast<const char16_t*>(wide[index])));
  }
  LocalFree(static_cast<HLOCAL>(wide));
  return CLIArgList::Parse(args);
}
#endif

int ReportEntryFailure(const char* what) noexcept {
  std::fprintf(stderr, "unhandled exception, %s\n", what);
  return 1;
}

void ResetAppState() {
  g_request.store(0);
  std::lock_guard lock(g_state_mutex);
  g_wake = {};
  g_restart.reset();
  ResetTeardown();
}

void SetShutdownWake(rex::MoveOnlyFunction<void()> wake) {
  std::lock_guard lock(g_state_mutex);
  g_wake = std::move(wake);
}

std::optional<i32> GetRequestedExitCode() {
  const u64 request = g_request.load();
  if (request == 0) {
    return std::nullopt;
  }
  return static_cast<i32>(static_cast<u32>(request));
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

[[nodiscard]] Result<void> RelaunchProcess(std::span<const std::string> args) {
  return g_test_relaunch ? g_test_relaunch(args) : platform::Relaunch(args);
}

bool CanRelaunchProcess() {
  return g_test_relaunch ? g_test_can_relaunch : platform::CanRelaunch();
}

}  // namespace detail

App::~App() = default;

void App::RequireModules(Config&) {}

Result<void> App::OnConfigure(Config&) {
  return Ok();
}

std::span<const CLIAction> App::GetActions() const {
  return {};
}

Result<void> App::OnAction(const CLIAction&, const CLIOption&) {
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
  REXLOG_ERROR(core, "{}", message);
}

platform::CrashReporterOptions App::GetCrashOptions() {
  return {};
}

const CLIArgList& App::GetArgs() const {
  return args_;
}

API& App::GetAPI() {
  return *api_;
}

Config& App::GetConfig() {
  return config_;
}

Result<void> App::Fail(std::string_view step, Error error) {
  failed_ = true;
  ReportError(std::format("{} failed: {}", step, error.GetDescription()));
  return std::unexpected(std::move(error));
}

Result<void> App::Init(const CLIArgList& args) {
  if (stage_ != Stage::kNone) {
    return Err<void>(ErrorCategory::kRuntime, "App::Init called twice");
  }
  args_ = args;
  stage_ = Stage::kConfigured;
  detail::ResetAppState();
  InstallSignalHandlers();
  if (auto crash = platform::InstallCrashReporter({}); !crash) {
    return Fail("crash reporter", crash.error());
  }

  config_.log.file.enabled = true;
  if (auto configured = OnConfigure(config_); !configured) {
    return Fail("OnConfigure", configured.error());
  }
  if (config_.log.file.name.empty()) {
    config_.log.file.name = platform::GetExecutablePath().stem().string();
  }
  if (config_.log.file.folder.empty()) {
    config_.log.file.folder = platform::GetExecutableFolder() / "logs";
  }
  const std::span<const CLIAction> actions = GetActions();
  if (auto valid = ValidateCLIActions(actions); !valid) {
    return Fail("command line action", valid.error());
  }
  std::vector<CLIActionHit> hits;
  if (auto unknown = ParseCommandLine(config_, args_, actions, &hits); !unknown.empty()) {
    return Fail("command line",
                Error(ErrorCategory::kConfig, "unknown option " + JoinOptions(unknown)));
  }
  for (const CLIActionHit& hit : hits) {
    if (auto handled = OnAction(hit.action, hit.option); !handled) {
      return Fail("command line action", handled.error());
    }
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
  RequireModules(config_);
  if (auto crash = platform::InstallCrashReporter(GetCrashOptions()); !crash) {
    return Fail("crash reporter", crash.error());
  }

  auto& loader = detail::ModuleLoader::GetInstance();
  auto api = loader.Connect(config_);
  if (!api) {
    return Fail("module connect", api.error());
  }
  api_ = *api;
  stage_ = Stage::kConnected;

  if (api_->Get<cvar::ICvarRegistry>()->Get("perf.profiler").value_or("false") == "true") {
    platform::Profiler::Startup();
  }
  BeginTimerResolution();
  timer_resolution_ = true;

  if (auto initialized = OnInit(loader.GetMutableConfig(), *api_); !initialized) {
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
  const bool loaded = stage_ == Stage::kConnected || stage_ == Stage::kInitialized;
  auto& loader = detail::ModuleLoader::GetInstance();
  if (loaded) {
    loader.ShutdownModules();
  }
  api_ = nullptr;
  platform::Profiler::Shutdown();
  if (timer_resolution_) {
    EndTimerResolution();
  }
  platform::detail::ShutdownTimerQueue();
  if (loaded) {
    loader.CloseLibraries();
  }
  lock_.reset();

  if (auto requested = detail::GetRequestedExitCode()) {
    final_code_ = *requested;
  } else if (failed_) {
    final_code_ = 1;
  } else {
    final_code_ = run_code_.value_or(0);
  }
  if (auto restart = detail::TakeRestartArgs(); restart && detail::CanRelaunchProcess()) {
    std::vector<std::string> relaunch_args = *restart;
    if (relaunch_args.empty()) {
      relaunch_args.assign(args_.GetAll().begin(), args_.GetAll().end());
    }
    if (auto relaunched = detail::RelaunchProcess(relaunch_args); !relaunched) {
      REXLOG_ERROR(core, "relaunch failed: {}", relaunched.error().GetDescription());
    }
  }
  RemoveSignalHandlers();
  platform::UninstallCrashReporter();
  if (loaded) {
    detail::StopLogger();
  }
  detail::SetShutdownWake({});
  stage_ = Stage::kShutdown;
  FinishTeardown();
  return final_code_;
}

}  // namespace rex
