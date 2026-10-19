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

#include <algorithm>
#include <iterator>

#include <rex/assert.h>
#include <rex/math.h>

namespace rex::platform {

namespace {

constexpr DWORD kSetThreadNameException = 0x406D1388;
void* g_veh_handle = nullptr;

constexpr size_t kMaxHandlerCount = 8;

struct HandlerSlot {
  ExceptionHandler::Handler fn = nullptr;
  void* data = nullptr;
};

HandlerSlot g_handlers[kMaxHandlerCount];

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  if (ex_info->ExceptionRecord->ExceptionCode == kSetThreadNameException) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  thread_context.rip = ex_info->ContextRecord->Rip;
  thread_context.eflags = ex_info->ContextRecord->EFlags;
  std::memcpy(thread_context.int_registers, &ex_info->ContextRecord->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ex_info->ContextRecord->Xmm0,
              sizeof(thread_context.xmm_registers));

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
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < rex::CountOf(g_handlers) && g_handlers[i].fn; ++i) {
    if (g_handlers[i].fn(&ex, g_handlers[i].data)) {
      ex_info->ContextRecord->Rip = thread_context.rip;
      ex_info->ContextRecord->EFlags = thread_context.eflags;
      u32 modified_register_index;
      u16 modified_int_registers_remaining = ex.GetModifiedIntRegisters();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        (&ex_info->ContextRecord->Rax)[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
      u16 modified_xmm_registers_remaining = ex.GetModifiedXMMRegisters();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&ex_info->ContextRecord->Xmm0 + modified_register_index,
                    &thread_context.xmm_registers[modified_register_index], sizeof(Vec128T));
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!g_veh_handle) {
    g_veh_handle = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);
  }

  auto slot = std::ranges::find_if(g_handlers, [](const auto& handler) { return !handler.fn; });
  if (slot != std::end(g_handlers)) {
    *slot = {fn, data};
    return;
  }
  REX_ASSERT_ALWAYS("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::CountOf(g_handlers); ++i) {
    if (g_handlers[i].fn == fn && g_handlers[i].data == data) {
      for (; i < rex::CountOf(g_handlers) - 1; ++i) {
        g_handlers[i] = g_handlers[i + 1];
      }
      g_handlers[i].fn = nullptr;
      g_handlers[i].data = nullptr;
      break;
    }
  }

  if (std::ranges::none_of(g_handlers, [](const auto& handler) { return handler.fn; })) {
    if (g_veh_handle) {
      RemoveVectoredExceptionHandler(g_veh_handle);
      g_veh_handle = nullptr;
    }
  }
}

}  // namespace rex::platform

#endif  // REX_PLATFORM_WIN32
