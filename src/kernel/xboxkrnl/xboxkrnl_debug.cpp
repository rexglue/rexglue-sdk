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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
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

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
struct X_THREADNAME_INFO {
  be_u32 type;
  be_u32 name_ptr;
  be_u32 thread_id;
  be_u32 flags;
};
REX_STATIC_ASSERT_SIZE(X_THREADNAME_INFO, 0x10);

void HandleSetThreadName(ppc_ptr_t<X_EXCEPTION_RECORD> record) {
  // SetThreadName. FFS.
  // https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx

  // TODO(benvanik): check record->number_parameters to make sure it's a
  // correct size.
  auto thread_info = reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  REX_ASSERT(thread_info->type == 0x1000);

  if (!thread_info->name_ptr) {
    REXLOG_DEBUG(kernel, "SetThreadName called with null name_ptr");
    return;
  }

  // 4D5307D6 (and its demo) has a bug where it ends up passing freed memory for
  // the name, so at the point of SetThreadName it's filled with junk.

  // TODO(gibbed): cvar for thread name encoding for conversion, some games use
  // SJIS and there's no way to automatically know this.
  auto name = std::string(
      runtime::Current().memory()->TranslateVirtual<const char*>(thread_info->name_ptr));
  std::replace_if(name.begin(), name.end(), [](auto c) { return c < 32 || c > 127; }, '?');

  ThreadPin thread;
  if (thread_info->thread_id == -1) {
    // Current thread.
    thread =
        runtime::Current().thread_registry().Find(ThreadRecord::GetCurrentThread()->thread_id());
  } else {
    // Lookup thread by ID.
    thread = runtime::Current().thread_registry().Find(thread_info->thread_id);
  }

  if (thread) {
    REXLOG_DEBUG(kernel, "SetThreadName({}, {})", thread->thread_id(), name);
    thread->set_name(name);
  }

  // TODO(benvanik): unwinding required here?
}

struct x_PMD {
  be_i32 mdisp;
  be_i32 pdisp;
  be_i32 vdisp;
};

struct x_s__CatchableType {
  be_u32 properties;
  be_u32 type_ptr;
  x_PMD this_displacement;
  be_i32 size_or_offset;
  be_u32 copy_function_ptr;
};

struct x_s__CatchableTypeArray {
  be_i32 number_catchable_types;
  be_u32 catchable_type_ptrs[1];
};

struct x_s__ThrowInfo {
  be_u32 attributes;
  be_u32 unwind_ptr;
  be_u32 forward_compat_ptr;
  be_u32 catchable_type_array_ptr;
};

void HandleCppException(ppc_ptr_t<X_EXCEPTION_RECORD> record) {
  // C++ exception.
  // https://blogs.msdn.com/b/oldnewthing/archive/2010/07/30/10044061.aspx
  // http://www.drdobbs.com/visual-c-exception-handling-instrumentat/184416600
  // http://www.openrce.org/articles/full_view/21

  REX_ASSERT(record->number_parameters == 3);
  REX_ASSERT(record->exception_information[0] == 0x19930520);

  auto thrown_ptr = record->exception_information[1];
  auto thrown = runtime::Current().memory()->TranslateVirtual(thrown_ptr);
  auto vftable_ptr = *reinterpret_cast<be_u32*>(thrown);

  auto throw_info_ptr = record->exception_information[2];
  auto throw_info = runtime::Current().memory()->TranslateVirtual<x_s__ThrowInfo*>(throw_info_ptr);
  auto catchable_types = runtime::Current().memory()->TranslateVirtual<x_s__CatchableTypeArray*>(
      throw_info->catchable_type_array_ptr);

  rex::platform::Break();
}

void RtlRaiseException_entry(ppc_ptr_t<X_EXCEPTION_RECORD> record) {
  switch (record->code) {
    case 0x406D1388: {
      HandleSetThreadName(record);
      return;
    }
    case 0xE06D7363: {
      HandleCppException(record);
      return;
    }
  }

  // TODO(benvanik): unwinding.
  // This is going to suck.
  rex::platform::Break();
}

void KeBugCheckEx_entry(u32 code, u32 param1, u32 param2, u32 param3, u32 param4) {
  REXLOG_DEBUG(kernel, "*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})", code, param1,
               param2, param3, param4);
  fflush(stdout);
  rex::platform::Break();
  REX_ASSERT_ALWAYS();
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
