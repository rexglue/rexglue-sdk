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
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/xenumerator.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <fmt/format.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t XamEnumerate(uint32_t handle, uint32_t flags, mapped_void buffer_ptr, uint32_t buffer_size,
                      uint32_t* items_returned, uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto& runtime = runtime::Current();
  if (!runtime.object_table().Get<EnumeratorObject>(handle)) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [runtime = &runtime, handle, buffer_ptr](uint32_t& extended_error,
                                                      uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    auto e = runtime->object_table().Get<EnumeratorObject>(handle);
    if (!e) {
      result = X_ERROR_INVALID_HANDLE;
    } else if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->write_items(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    runtime::Current().dispatcher().CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                       mapped_u32 items_returned, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result =
      XamEnumerate(handle, flags, buffer, buffer_length,
                   !overlapped.guest_address() ? &dummy : nullptr, overlapped.guest_address());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

u32 XamCreateEnumeratorHandle_entry(u32 unk1, u32 unk2, u32 unk3, u32 unk4, u32 unk5, u32 unk6,
                                    u32 unk7, u32 unk8) {
  return X_ERROR_INVALID_PARAMETER;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 out_object_ptr) {
  REX_KERNEL_LOOKUP(e, EnumeratorObject, handle);

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  (void)runtime::Current().object_table().RetainPrimary(e.slot());

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e.guest_object();
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
