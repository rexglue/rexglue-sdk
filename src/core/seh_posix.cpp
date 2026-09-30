/**
 * @file        core/seh_posix.cpp
 * @brief       POSIX platform SEH implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>
#include <cstdlib>

#include <rex/platform/exceptions.h>

namespace rex::platform {

namespace {
std::atomic<bool> g_seh_initialized{false};
}  // namespace

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(u32 /*code*/, void* /*ep*/) {
  return 0;
}

static void signal_handler(int sig, siginfo_t* info, void* /*ucontext*/) {
  if (!tls_seh_active) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
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

  // libunwind unwinds through the signal frame.
  throw SehException(code, address);
}

[[noreturn]] void seh_rethrow() {
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
  signal(sig, SIG_DFL);
  raise(sig);
  abort();
}

void seh_initialize() {
  if (g_seh_initialized.exchange(true)) {
    return;
  }

  struct sigaction sa;
  sa.sa_sigaction = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;

  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform
