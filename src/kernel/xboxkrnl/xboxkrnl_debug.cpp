/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/dbg.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/runtime/thread_registry.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xexception.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

void DbgBreakPoint_entry() {
  rex::platform::Break();
}

struct X_THREADNAME_INFO {
  be_u32 type;
  be_u32 name_ptr;
  be_u32 thread_id;
  be_u32 flags;
};
REX_STATIC_ASSERT_SIZE(X_THREADNAME_INFO, 0x10);

constexpr size_t kMaxThreadNameLength = 256;

void HandleSetThreadName(GuestPtrT<X_EXCEPTION_RECORD> record) {
  auto thread_info = reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  if (thread_info->type != 0x1000) {
    return;
  }

  if (!thread_info->name_ptr) {
    REXLOG_DEBUG(kernel, "SetThreadName called with null name_ptr");
    return;
  }

  // A title may pass freed memory for the name, which holds junk.
  const char* name_data =
      runtime::GetCurrent().GetMemory()->TranslateVirtual<const char*>(thread_info->name_ptr);
  auto name = std::string(name_data, strnlen(name_data, kMaxThreadNameLength));
  std::replace_if(name.begin(), name.end(), [](auto c) { return c < 32 || c > 127; }, '?');

  ThreadPin thread;
  if (thread_info->thread_id == ~u32{0}) {
    thread = runtime::GetCurrent().GetThreadRegistry().Find(
        KernelThread::GetCurrentThread()->GetThreadId());
  } else {
    thread = runtime::GetCurrent().GetThreadRegistry().Find(thread_info->thread_id);
  }

  if (thread) {
    REXLOG_DEBUG(kernel, "SetThreadName({}, {})", thread->GetThreadId(), name);
    thread->SetName(name);
  }
}

void HandleCppException(GuestPtrT<X_EXCEPTION_RECORD> record) {
  REX_ASSERT(record->number_parameters == 3);
  REX_ASSERT(record->exception_information[0] == 0x19930520);

  rex::platform::Break();
}

void RtlRaiseException_entry(GuestPtrT<X_EXCEPTION_RECORD> record) {
  switch (record->code) {
    case 0x406D1388: {
      HandleSetThreadName(record);
      return;
    }
    case 0xE06D7363: {
      HandleCppException(record);
      return;
    }
    default:
      break;
  }

  rex::platform::Break();
}

void KeBugCheckEx_entry(u32 code, u32 param1, u32 param2, u32 param3, u32 param4) {
  REXLOG_ERROR(kernel, "*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})", code, param1,
               param2, param3, param4);
  fflush(stdout);
  if (rex::platform::IsDebuggerAttached()) {
    rex::platform::Break();
  }
  runtime::GetCurrent().ExitTitle(TitleExitReason::kBugCheck);
}

void KeBugCheck_entry(u32 code) {
  KeBugCheckEx_entry(code, 0, 0, 0, 0);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__DbgBreakPoint, rex::kernel::xboxkrnl::DbgBreakPoint_entry)
REX_EXPORT(__imp__RtlRaiseException, rex::kernel::xboxkrnl::RtlRaiseException_entry)
REX_EXPORT(__imp__KeBugCheckEx, rex::kernel::xboxkrnl::KeBugCheckEx_entry)
REX_EXPORT(__imp__KeBugCheck, rex::kernel::xboxkrnl::KeBugCheck_entry)

REX_EXPORT_STUB(__imp__DbgBreakPointWithStatus);
REX_EXPORT_STUB(__imp__DbgPrompt);
REX_EXPORT_STUB(__imp__DbgLoadImageSymbols);
REX_EXPORT_STUB(__imp__DbgUnLoadImageSymbols);
REX_EXPORT_STUB(__imp__DmPrintData);

REX_EXPORT_STUB(__imp__DumpGetRawDumpInfo);
REX_EXPORT_STUB(__imp__DumpRegisterDedicatedDataBlock);
REX_EXPORT_STUB(__imp__DumpSetCollectionFacility);
REX_EXPORT_STUB(__imp__DumpUpdateDumpSettings);
REX_EXPORT_STUB(__imp__DumpWriteDump);
REX_EXPORT_STUB(__imp__DumpXitThread);
REX_EXPORT_STUB(__imp__RtlAssert);
REX_EXPORT_STUB(__imp__RtlRaiseStatus);
REX_EXPORT_STUB(__imp__RtlRip);
