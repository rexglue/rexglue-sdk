#include <utility>

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

namespace rex::platform {

namespace {
std::atomic<bool> g_seh_initialized{false};
}  // namespace

static thread_local SehThreadState tls_seh_state;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(u32 code, void* ep) {
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
  u32 code = tls_seh_state.code;
  ULONG_PTR info[2] = {tls_seh_state.info[0], tls_seh_state.info[1]};

  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 2, info);
  } else {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
  std::unreachable();
}

void seh_initialize() {
  g_seh_initialized.store(true, std::memory_order_relaxed);
}

}  // namespace rex::platform
