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

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/runtime/dispatcher.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/xtypes.h>
#include "kernel/object_lookup.h"
#include "kernel/object_reference.h"

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <format>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

u32 XamEnumerate(u32 handle, u32 flags, MappedVoid buffer_ptr, u32* items_returned,
                 u32 overlapped_ptr) {
  REX_ASSERT(flags == 0);

  auto& runtime = runtime::GetCurrent();
  if (!runtime.GetObjectTable().Get<EnumeratorObject>(handle)) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [runtime = &runtime, handle, buffer_ptr](u32& extended_error,
                                                      u32& length) -> X_RESULT {
    X_RESULT result;
    u32 item_count = 0;
    auto e = runtime->GetObjectTable().Get<EnumeratorObject>(handle);
    if (!e) {
      result = X_ERROR_INVALID_HANDLE;
    } else if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->write_items(buffer_ptr.GetGuestAddress(), buffer_ptr.As<u8*>(), &item_count);
    }
    extended_error = XHResultFromWin32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    REX_ASSERT(!overlapped_ptr);
    u32 extended_error;
    u32 item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    REX_ASSERT(!items_returned);
    runtime::GetCurrent().GetDispatcher().CompleteDeferred(
        overlapped_ptr, [run = std::move(run)]() mutable {
          u32 extended_error = 0;
          u32 length = 0;
          const X_RESULT result = run(extended_error, length);
          return runtime::CompletionOptions{
              .result = result == X_ERROR_SUCCESS ? result : X_ERROR_FUNCTION_FAILED,
              .extended_error = extended_error,
              .length = length};
        });
    return X_ERROR_IO_PENDING;
  } else {
    REX_ASSERT_ALWAYS();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, MappedVoid buffer, [[maybe_unused]] u32 buffer_length,
                       MappedU32 items_returned, GuestPtrT<XAM_OVERLAPPED> overlapped) {
  u32 dummy = 0;
  auto result =
      XamEnumerate(handle, flags, buffer, !overlapped.GetGuestAddress() ? &dummy : nullptr,
                   overlapped.GetGuestAddress());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

u32 XamCreateEnumeratorHandle_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                                    [[maybe_unused]] u32 unk3, [[maybe_unused]] u32 unk4,
                                    [[maybe_unused]] u32 unk5, [[maybe_unused]] u32 unk6,
                                    [[maybe_unused]] u32 unk7, [[maybe_unused]] u32 unk8) {
  return X_ERROR_INVALID_PARAMETER;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, MappedU32 out_object_ptr) {
  REX_KERNEL_LOOKUP(e, EnumeratorObject, handle);

  auto referenced =
      ReferencePrimary(runtime::GetCurrent().GetObjectTable(), e.GetSlot(), e.GetGuestObject());
  if (!referenced) {
    return ToXStatus(referenced);
  }

  if (out_object_ptr.GetGuestAddress()) {
    *out_object_ptr = *referenced;
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamEnumerate, rex::kernel::xam::XamEnumerate_entry)
REX_EXPORT(__imp__XamCreateEnumeratorHandle, rex::kernel::xam::XamCreateEnumeratorHandle_entry)
REX_EXPORT(__imp__XamGetPrivateEnumStructureFromHandle,
           rex::kernel::xam::XamGetPrivateEnumStructureFromHandle_entry)
