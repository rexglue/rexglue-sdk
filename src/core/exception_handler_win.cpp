/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/exception_handler.h>

#if REX_PLATFORM_WIN32

#include "platform_win.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>

#include <rex/assert.h>
#include <rex/math.h>

namespace rex::arch {

// Handle of the added VectoredExceptionHandler.
void* veh_handle_ = nullptr;
// Handle of the added VectoredContinueHandler.
void* vch_handle_ = nullptr;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  // Visual Studio SetThreadName.
  if (ex_info->ExceptionRecord->ExceptionCode == 0x406D1388) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  thread_context.rip = ex_info->ContextRecord->Rip;
  thread_context.eflags = ex_info->ContextRecord->EFlags;
  std::memcpy(thread_context.int_registers, &ex_info->ContextRecord->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ex_info->ContextRecord->Xmm0,
              sizeof(thread_context.xmm_registers));

  // https://msdn.microsoft.com/en-us/library/ms679331(v=vs.85).aspx
  // https://msdn.microsoft.com/en-us/library/aa363082(v=vs.85).aspx
  Exception ex;
  switch (ex_info->ExceptionRecord->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case STATUS_ACCESS_VIOLATION: {
      Exception::AccessViolationOperation access_violation_operation;
      switch (ex_info->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
          access_violation_operation = Exception::AccessViolationOperation::kRead;
          break;
        case 1:
          access_violation_operation = Exception::AccessViolationOperation::kWrite;
          break;
        default:
          access_violation_operation = Exception::AccessViolationOperation::kUnknown;
          break;
      }
      ex.InitializeAccessViolation(&thread_context,
                                   ex_info->ExceptionRecord->ExceptionInformation[1],
                                   access_violation_operation);
    } break;
    default:
      // Unknown/unhandled type.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
      ex_info->ContextRecord->Rip = thread_context.rip;
      ex_info->ContextRecord->EFlags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        (&ex_info->ContextRecord->Rax)[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&ex_info->ContextRecord->Xmm0 + modified_register_index,
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// --- Last-chance crash diagnostics -----------------------------------------
// The VEH above only claims the two exception kinds guest memory emulation
// needs; everything else returns EXCEPTION_CONTINUE_SEARCH and the process dies
// with nothing written down. That is how a recompiled title can exit
// 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN, which is what __fastfail and an
// abort() from std::terminate both surface as) with an empty log and no clue
// where it happened.
//
// So: name the corpse. A terminate handler reports the in-flight C++ exception,
// an unhandled-exception filter reports the code and faulting address, and both
// write a return-address backtrace as module+RVA -- which is enough to point
// llvm-symbolizer at the port's PDB and get real frames back.
//
// Written to a file rather than the log because the logger may be exactly what
// is broken, and appended so a crash is never overwritten by the next run.

static void RexWriteCrashReport(const char* why, const char* detail, CONTEXT* ctx) {
  FILE* f = std::fopen("rexglue-crash.txt", "a");
  if (!f) {
    return;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(f, "\n=== %04u-%02u-%02u %02u:%02u:%02u  pid %lu  thread %lu ===\n", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(),
               GetCurrentThreadId());
  std::fprintf(f, "%s%s%s\n", why, detail && *detail ? ": " : "", detail ? detail : "");
  if (ctx) {
    std::fprintf(f, "  rip=%016llX rsp=%016llX rbp=%016llX\n", (unsigned long long)ctx->Rip,
                 (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp);
  }

  void* frames[62];
  USHORT n = CaptureStackBackTrace(0, (USHORT)rex::countof(frames), frames, nullptr);
  std::fprintf(f, "  backtrace (%u frames, module+RVA -- feed to llvm-symbolizer):\n", n);
  for (USHORT i = 0; i < n; ++i) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)frames[i], &mod) &&
        mod) {
      char full[MAX_PATH];
      if (GetModuleFileNameA(mod, full, MAX_PATH)) {
        const char* slash = std::strrchr(full, '\\');
        std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : full);
      }
    }
    std::fprintf(f, "    %2u  %-24s +0x%llX\n", i, name,
                 (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
  }
  std::fflush(f);
  std::fclose(f);
}

static void RexTerminateHandler() {
  // Recover what was thrown. An exception escaping a noexcept boundary is the
  // most likely way this runtime reaches terminate, and its type and message are
  // the whole diagnosis -- as they were for the guest string_view that XamContent
  // handed to the dispatch thread.
  const char* detail = "no in-flight exception";
  std::string msg;
  if (std::current_exception()) {
    try {
      std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
      msg = std::string(typeid(e).name()) + " -- " + e.what();
      detail = msg.c_str();
    } catch (...) {
      detail = "non-std exception";
    }
  }
  RexWriteCrashReport("std::terminate called", detail, nullptr);
  std::_Exit(3);
}

static LONG WINAPI RexUnhandledExceptionFilter(EXCEPTION_POINTERS* ex) {
  char detail[128];
  std::snprintf(detail, sizeof(detail), "code 0x%08lX at %p", ex->ExceptionRecord->ExceptionCode,
                ex->ExceptionRecord->ExceptionAddress);
  RexWriteCrashReport("unhandled exception", detail, ex->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!veh_handle_) {
    veh_handle_ = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);
    std::set_terminate(RexTerminateHandler);
    SetUnhandledExceptionFilter(RexUnhandledExceptionFilter);

    if (IsDebuggerPresent()) {
      // TODO(benvanik): do we need a continue handler if a debugger is
      // attached?
      // vch_handle_ = AddVectoredContinueHandler(1, ExceptionHandlerCallback);
    }
  }

  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (veh_handle_) {
      RemoveVectoredExceptionHandler(veh_handle_);
      veh_handle_ = nullptr;
    }
    if (vch_handle_) {
      RemoveVectoredContinueHandler(vch_handle_);
      vch_handle_ = nullptr;
    }
  }
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_WIN32
