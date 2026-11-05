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

#include "xam/xam_state.h"
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/thread.h>
#include <rex/ppc/call.h>
#include "xam/xam_ui.h"
#include <rex/logging/log.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include "xam/enumerator_object.h"
#include <rex/kernel/types/status.h>

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <format>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

u32 EnumerateItems(u32 handle, u32 flags, MappedVoid buffer_ptr, u32* items_returned,
                   u32 overlapped_ptr) {
  REX_ASSERT(flags == 0);

  if (!CurrentTitle().objects.Get<EnumeratorObject>(handle)) {
    return kernel::X_ERROR_INVALID_HANDLE;
  }

  auto run = [handle, buffer_ptr](u32& extended_error, u32& length,
                                  bool& results_in_buffer) -> kernel::X_RESULT {
    kernel::X_RESULT result;
    u32 item_count = 0;
    auto e = CurrentTitle().objects.Get<EnumeratorObject>(handle);
    if (!e) {
      result = kernel::X_ERROR_INVALID_HANDLE;
    } else if (!buffer_ptr) {
      result = kernel::X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->write_items(buffer_ptr.GetGuestAddress(), buffer_ptr.As<u8*>(), &item_count);
      results_in_buffer = e->results_in_buffer;
    }
    extended_error = kernel::XHRESULTFromWin32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    REX_ASSERT(!overlapped_ptr);
    u32 extended_error;
    u32 item_count;
    bool results_in_buffer = false;
    kernel::X_RESULT result = run(extended_error, item_count, results_in_buffer);
    *items_returned = result == kernel::X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    REX_ASSERT(!items_returned);
    CurrentTitle().overlapped_dispatcher.CompleteDeferred(
        overlapped_ptr, [run = std::move(run)]() mutable {
          u32 extended_error = 0;
          u32 length = 0;
          bool results_in_buffer = false;
          const kernel::X_RESULT result = run(extended_error, length, results_in_buffer);
          return kernel::CompletionOptions{.result = result == kernel::X_ERROR_SUCCESS
                                                         ? result
                                                         : kernel::X_ERROR_FUNCTION_FAILED,
                                           .extended_error = extended_error,
                                           .length = results_in_buffer ? 0 : length};
        });
    return kernel::X_ERROR_IO_PENDING;
  } else {
    REX_ASSERT_ALWAYS();
    return kernel::X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, MappedVoid buffer, [[maybe_unused]] u32 buffer_length,
                       MappedU32 items_returned, GuestPtrT<kernel::XOVERLAPPED> overlapped) {
  u32 item_count = 0;
  auto result =
      EnumerateItems(handle, flags, buffer, !overlapped.GetGuestAddress() ? &item_count : nullptr,
                     overlapped.GetGuestAddress());
  if (!overlapped && items_returned) {
    *items_returned = item_count;
  }
  return result;
}

u32 XamCreateEnumeratorHandle_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                                    [[maybe_unused]] u32 unk3, [[maybe_unused]] u32 unk4,
                                    [[maybe_unused]] u32 unk5, [[maybe_unused]] u32 unk6,
                                    [[maybe_unused]] u32 unk7, [[maybe_unused]] u32 unk8) {
  return kernel::X_ERROR_INVALID_PARAMETER;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, MappedU32 out_object_ptr) {
  auto e = CurrentTitle().objects.Get<EnumeratorObject>(handle);
  if (!e) {
    return kernel::X_STATUS_INVALID_HANDLE;
  }

  if (auto retained = CurrentTitle().objects.Reference(e.GetSlot()); !retained) {
    return kernel::ToXStatus(retained);
  }

  if (out_object_ptr.GetGuestAddress()) {
    *out_object_ptr = e.GetGuestAddress();
  }

  return kernel::X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamEnumerate, rex::xam::XamEnumerate_entry)
REX_XAM_BODY(__imp__XamCreateEnumeratorHandle, rex::xam::XamCreateEnumeratorHandle_entry)
REX_XAM_BODY(__imp__XamGetPrivateEnumStructureFromHandle,
             rex::xam::XamGetPrivateEnumStructureFromHandle_entry)
