/**
 * @file        core/seh_posix.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

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

static thread_local SehThreadState tls_seh_state;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(u32 /*code*/, void* /*ep*/) {
  return 0;
}

[[noreturn]] static void ResumeScope(SehScope& scope) {
  tls_seh_state.top = scope.outer;
  siglongjmp(scope.env, 1);
}

static void signal_handler(int sig, siginfo_t* info, void* ucontext) {
  SehScope* scope = tls_seh_state.top;
  if (!scope || scope->host_depth != tls_seh_state.host_depth ||
      !IsRecompiledCode(detail::SignalPc(ucontext))) {
    if (struct sigaction* previous = PreviousAction(sig)) {
      detail::ForwardSignal(*previous, sig, info, ucontext);
      return;
    }
    detail::ReportFatalSignal(sig, info, ucontext);
    detail::DieWithDefaultDisposition(sig);
  }

  SehException::Code code;
  switch (sig) {
    case SIGSEGV:
      code = SehException::ACCESS_VIOLATION;
      break;
    case SIGBUS:
      code = SehException::IN_PAGE_ERROR;
      break;
    case SIGFPE:
      code = SehException::FLOAT_DIVIDE_BY_ZERO;
      break;
    case SIGILL:
      code = SehException::ILLEGAL_INSTRUCTION;
      break;
    default:
      code = SehException::UNKNOWN;
      break;
  }

  uintptr_t address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

  tls_seh_state.code = static_cast<u32>(code);
  tls_seh_state.info[0] = 0;
  tls_seh_state.info[1] = address;

  ResumeScope(*scope);
}

[[noreturn]] void seh_rethrow() {
  if (SehScope* scope = tls_seh_state.top; scope && scope->host_depth == tls_seh_state.host_depth) {
    ResumeScope(*scope);
  }
  u32 code = tls_seh_state.code;
  int sig;
  switch (code) {
    case SehException::ACCESS_VIOLATION:
      sig = SIGSEGV;
      break;
    case SehException::IN_PAGE_ERROR:
      sig = SIGBUS;
      break;
    case SehException::FLOAT_DIVIDE_BY_ZERO:
    case SehException::INTEGER_DIVIDE_BY_ZERO:
      sig = SIGFPE;
      break;
    case SehException::ILLEGAL_INSTRUCTION:
      sig = SIGILL;
      break;
    default:
      abort();
  }
  detail::ReportFatalSignal(sig, nullptr, nullptr);
  detail::DieWithDefaultDisposition(sig);
}

void seh_initialize() {
  if (g_seh_initialized.exchange(true)) {
    return;
  }

  struct sigaction sa{};
  sa.sa_sigaction = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  for (size_t i = 0; i < kSEHSignals.size(); ++i) {
    sigaction(kSEHSignals[i], &sa, &g_previous[i]);
  }
}

}  // namespace rex::platform
