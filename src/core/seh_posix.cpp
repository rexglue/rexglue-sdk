#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>
#include <array>
#include <cstdlib>

#include <rex/platform/exceptions.h>
#include <rex/ppc/recompiled_code.h>

#include "core/crash_report.h"
#include "core/signal_context.h"

namespace rex::platform {

namespace {
std::atomic<bool> g_seh_initialized{false};

constexpr std::array kSEHSignals = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
std::array<struct sigaction, kSEHSignals.size()> g_previous{};

struct sigaction* PreviousAction(int signal_number) {
  for (size_t i = 0; i < kSEHSignals.size(); ++i) {
    if (kSEHSignals[i] == signal_number) {
      return &g_previous[i];
    }
  }
  return nullptr;
}
}  // namespace

static thread_local SEHThreadState g_seh_state;

SEHThreadState& GetSEHThreadState() {
  return g_seh_state;
}

int SEHFilter(u32 /*code*/, void* /*ep*/) {
  return 0;
}

[[noreturn]] static void ResumeScope(SEHScope& scope) {
  g_seh_state.top = scope.outer;
  siglongjmp(scope.env, 1);
}

static void SignalHandler(int sig, siginfo_t* info, void* ucontext) {
  SEHScope* scope = g_seh_state.top;
  if (!scope || scope->host_depth != g_seh_state.host_depth ||
      !IsRecompiledCode(detail::SignalPC(ucontext))) {
    if (struct sigaction* previous = PreviousAction(sig)) {
      detail::ForwardSignal(*previous, sig, info, ucontext);
      return;
    }
    detail::ReportFatalSignal(sig, info, ucontext);
    detail::DieWithDefaultDisposition(sig);
  }

  SEHException::Code code;
  switch (sig) {
    case SIGSEGV:
      code = SEHException::kAccessViolation;
      break;
    case SIGBUS:
      code = SEHException::kInPageError;
      break;
    case SIGFPE:
      code = SEHException::kFloatDivideByZero;
      break;
    case SIGILL:
      code = SEHException::kIllegalInstruction;
      break;
    default:
      code = SEHException::kUnknown;
      break;
  }

  uintptr_t address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

  g_seh_state.code = static_cast<u32>(code);
  g_seh_state.info[0] = 0;
  g_seh_state.info[1] = address;

  ResumeScope(*scope);
}

[[noreturn]] void SEHRethrow() {
  if (SEHScope* scope = g_seh_state.top; scope && scope->host_depth == g_seh_state.host_depth) {
    ResumeScope(*scope);
  }
  u32 code = g_seh_state.code;
  int sig;
  switch (code) {
    case SEHException::kAccessViolation:
      sig = SIGSEGV;
      break;
    case SEHException::kInPageError:
      sig = SIGBUS;
      break;
    case SEHException::kFloatDivideByZero:
    case SEHException::kIntegerDivideByZero:
      sig = SIGFPE;
      break;
    case SEHException::kIllegalInstruction:
      sig = SIGILL;
      break;
    default:
      abort();
  }
  detail::ReportFatalSignal(sig, nullptr, nullptr);
  detail::DieWithDefaultDisposition(sig);
}

void SEHInitialize() {
  if (g_seh_initialized.exchange(true)) {
    return;
  }

  struct sigaction sa{};
  sa.sa_sigaction = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  for (size_t i = 0; i < kSEHSignals.size(); ++i) {
    sigaction(kSEHSignals[i], &sa, &g_previous[i]);
  }
}

}  // namespace rex::platform
