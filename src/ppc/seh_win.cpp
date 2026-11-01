#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <rex/platform.h>
#include <rex/ppc/seh.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

namespace rex::platform {

namespace {
std::atomic<bool> g_seh_initialized{false};
}  // namespace

static thread_local SEHThreadState g_seh_state;

SEHThreadState& GetSEHThreadState() {
  return g_seh_state;
}

int SEHFilter(u32 code, void* ep) {
  auto* pointers = static_cast<EXCEPTION_POINTERS*>(ep);
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
      g_seh_state.code = code;
      if (pointers && pointers->ExceptionRecord) {
        g_seh_state.info[0] = pointers->ExceptionRecord->ExceptionInformation[0];
        g_seh_state.info[1] = pointers->ExceptionRecord->ExceptionInformation[1];
      }
      return EXCEPTION_EXECUTE_HANDLER;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}

[[noreturn]] void SEHRethrow() {
  u32 code = g_seh_state.code;
  ULONG_PTR info[2] = {g_seh_state.info[0], g_seh_state.info[1]};

  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 2, info);
  } else {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
  std::unreachable();
}

void SEHInitialize() {
  g_seh_initialized.store(true, std::memory_order_relaxed);
}

}  // namespace rex::platform
