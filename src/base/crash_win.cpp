#include <rex/platform/process.h>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32

#include <algorithm>
#include <bit>
#include <csignal>
#include <cstring>
#include <format>

#include "base/crash_report.h"
#include "platform_win.h"

namespace rex::platform {

namespace {

constexpr DWORD kCPPException = 0xE06D7363;
constexpr DWORD kStackBufferOverrun = 0xC0000409;
constexpr DWORD kHeapCorruption = 0xC0000374;
constexpr u32 kBacktraceFrames = 32;

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
_crt_signal_t g_previous_abort = SIG_DFL;

std::string_view ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "IN_PAGE_ERROR";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
      return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:
      return "BREAKPOINT";
    case kStackBufferOverrun:
      return "STACK_BUFFER_OVERRUN";
    case kHeapCorruption:
      return "HEAP_CORRUPTION";
    default:
      return "EXCEPTION";
  }
}

std::string_view AccessName(ULONG_PTR kind) {
  switch (kind) {
    case 0:
      return "read";
    case 1:
      return "write";
    case 8:
      return "execute";
    default:
      return "unknown";
  }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == kCPPException && g_previous_filter) {
    return g_previous_filter(info);
  }
  if (!detail::BeginReport()) {
    detail::WaitForOtherReport();
    return EXCEPTION_CONTINUE_SEARCH;
  }
  HostThreadContext context{};
  context.rip = info->ContextRecord->Rip;
  context.eflags = info->ContextRecord->EFlags;
  std::memcpy(context.int_registers, &info->ContextRecord->Rax, sizeof(context.int_registers));
  std::memcpy(context.xmm_registers, &info->ContextRecord->Xmm0, sizeof(context.xmm_registers));

  detail::FaultReport fault;
  fault.kind = ExceptionName(code);
  fault.code = code;
  fault.pc = context.rip;
  fault.context = &context;
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      info->ExceptionRecord->NumberParameters >= 2) {
    fault.fault_address = info->ExceptionRecord->ExceptionInformation[1];
    fault.access = AccessName(info->ExceptionRecord->ExceptionInformation[0]);
  }
  detail::ReportFault(fault);
  detail::EndReport();
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void AbortSignal(int) {
  if (detail::BeginReport()) {
    detail::ReportMessage("SIGABRT (abort, failed assertion or uncaught exception)");
    detail::EndReport();
  } else {
    detail::WaitForOtherReport();
  }
}

}  // namespace

namespace detail {

bool IsReadable(u64 address, u64 size) {
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  const u64 end = address + size;
  while (address < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(std::bit_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || !(info.Protect & kReadable) || (info.Protect & PAGE_GUARD)) {
      return false;
    }
    address = std::bit_cast<u64>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

std::string_view DescribeCode(u64 address, std::span<char> out) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          std::bit_cast<LPCWSTR>(address), &module)) {
    return {};
  }
  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
  std::string_view name(path, length);
  if (const size_t slash = name.find_last_of("\\/"); slash != std::string_view::npos) {
    name.remove_prefix(slash + 1);
  }
  const auto result =
      std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()), "{}+{:#010x}", name,
                       address - reinterpret_cast<u64>(module));
  return {out.data(), std::min(static_cast<size_t>(result.size), out.size())};
}

void LogBacktrace() {
  void* frames[kBacktraceFrames] = {};
  const u16 count = RtlCaptureStackBackTrace(0, kBacktraceFrames, frames, nullptr);
  for (u16 i = 0; i < count; ++i) {
    WriteBacktraceFrame(i, reinterpret_cast<u64>(frames[i]));
  }
}

[[nodiscard]] Result<void> InstallPlatform() {
  g_previous_filter = SetUnhandledExceptionFilter(CrashFilter);
  g_previous_abort = std::signal(SIGABRT, AbortSignal);
  if (g_previous_abort == SIG_ERR) {
    SetUnhandledExceptionFilter(g_previous_filter);
    g_previous_abort = SIG_DFL;
    return Err(ErrorCategory::kPlatform, "SIGABRT handler could not be installed");
  }
  return Ok();
}

void UninstallPlatform() {
  SetUnhandledExceptionFilter(g_previous_filter);
  g_previous_filter = nullptr;
  std::signal(SIGABRT, g_previous_abort);
  g_previous_abort = SIG_DFL;
}

}  // namespace detail

void PrepareThreadForCrash() {
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
}

}  // namespace rex::platform

#endif
