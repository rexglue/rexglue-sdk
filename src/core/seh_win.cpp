/**
 * @file        core/seh_win.cpp
 * @brief       Windows platform SEH implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

namespace rex::platform {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(uint32_t code, void* ep) {
  auto* pointers = static_cast<EXCEPTION_POINTERS*>(ep);
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
      tls_seh_state.code = code;
      if (pointers && pointers->ExceptionRecord) {
        tls_seh_state.info[0] = pointers->ExceptionRecord->ExceptionInformation[0];
        tls_seh_state.info[1] = pointers->ExceptionRecord->ExceptionInformation[1];
      }
      return EXCEPTION_EXECUTE_HANDLER;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}

[[noreturn]] void seh_rethrow() {
  uint32_t code = tls_seh_state.code;
  ULONG_PTR info[2] = {tls_seh_state.info[0], tls_seh_state.info[1]};

  // For access violations, include the extra info (read/write flag and address)
  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 2, info);
  } else {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
  // RaiseException doesn't return, but compiler may not know that
#ifdef __clang__
  __builtin_unreachable();
#else
  __assume(false);
#endif
}

void seh_initialize() {
  // Native SEH needs no signal handler setup, but mark as initialized
  g_seh_initialized.store(true, std::memory_order_relaxed);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform
