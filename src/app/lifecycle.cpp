#include <atomic>
#include <csignal>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <rex/app/app.h>
#include <rex/platform/process.h>
#include <rex/platform/thread.h>
#include <rex/utility.h>

#include "app/lifecycle.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <pthread.h>

#include <memory>
#endif

namespace rex::app {
namespace {

constexpr u64 kRequestedBit = u64{1} << 32;
std::atomic<u64> g_request{0};
std::mutex g_state_mutex;
rex::MoveOnlyFunction<void()> g_wake;
std::optional<std::vector<std::string>> g_restart;
detail::RelaunchFunction g_test_relaunch = nullptr;
detail::StartupBoxFunction g_test_startup_box = nullptr;
bool g_test_can_relaunch = true;
const void* g_init_owner = nullptr;

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

void ResetTeardown() {
  g_teardown_done = false;
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

void ResetTeardown() {}
#endif

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
void InstallSignalHandlers() {
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
}

void RemoveSignalHandlers() {
  SetConsoleCtrlHandler(ConsoleHandler, FALSE);
}

void FinishTeardown() {
  g_teardown_done = true;
  g_teardown_done.notify_all();
}
#else
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

void FinishTeardown() {}
#endif

void ResetAppState() {
  g_request.store(0);
  std::lock_guard lock(g_state_mutex);
  g_wake = {};
  g_restart.reset();
  ResetTeardown();
}

void SetInitOwner(const void* owner) {
  std::lock_guard lock(g_state_mutex);
  g_init_owner = owner;
}

bool ReleaseInitOwner(const void* owner) {
  std::lock_guard lock(g_state_mutex);
  if (g_init_owner != owner) {
    return false;
  }
  g_init_owner = nullptr;
  return true;
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

void SetStartupBoxForTest(StartupBoxFunction box) {
  g_test_startup_box = box;
}

void ShowStartupBox(MessageType type, std::string_view message) {
  if (g_test_startup_box) {
    g_test_startup_box(type, message);
    return;
  }
  rex::ShowMessageBox(type, message);
}

bool CanRelaunchProcess() {
  return g_test_relaunch ? g_test_can_relaunch : platform::CanRelaunch();
}

}  // namespace detail
}  // namespace rex::app
