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
#include "xam/xam_ui.h"
#include <rex/kernel/xboxkrnl.h>
#include <rex/logging/log_category.h>
#include "xam/categories.h"
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/content_manager.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  auto result = CurrentXAM().GetAppManager().DispatchMessageSync(app, message, arg1, arg2);
  if (result == kernel::X_ERROR_NOT_FOUND) {
    REXXAM_ERROR("XMsgInProcessCall: app {:08X} undefined", app);
  }
  return result;
}

u32 XMsgSystemProcessCall_entry(u32 app, u32 message, u32 buffer, u32 buffer_length) {
  auto result =
      CurrentXAM().GetAppManager().DispatchMessageAsync(app, message, buffer, buffer_length);
  if (result == kernel::X_ERROR_NOT_FOUND) {
    REXXAM_ERROR("XMsgSystemProcessCall: app {:08X} undefined", app);
  }
  return result;
}

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be_u32 unk_0;
  be_u32 unk_1;
};

kernel::X_HRESULT XMsgStartIORequestEx(u32 app, u32 message, u32 overlapped_ptr, u32 buffer_ptr,
                                       u32 buffer_length,
                                       [[maybe_unused]] XMSGSTARTIOREQUEST_UNKNOWNARG* unknown) {
  auto result =
      CurrentXAM().GetAppManager().DispatchMessageAsync(app, message, buffer_ptr, buffer_length);
  if (result == kernel::X_E_NOTFOUND) {
    REXXAM_ERROR("XMsgStartIORequestEx: app {:08X} undefined", app);
    result = kernel::X_E_INVALIDARG;
    CurrentTitle().threads.GetCallingThread()->SetLastError(kernel::X_ERROR_NOT_FOUND);
  }
  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr, {.result = result});
    result = kernel::X_ERROR_IO_PENDING;
  }
  if (result == kernel::X_ERROR_SUCCESS || result == kernel::X_ERROR_IO_PENDING) {
    CurrentTitle().threads.GetCallingThread()->SetLastError(0);
  }
  return result;
}

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, GuestPtrT<kernel::XOVERLAPPED> overlapped_ptr,
                               u32 buffer_ptr, u32 buffer_length,
                               GuestPtrT<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return XMsgStartIORequestEx(app, message, overlapped_ptr.GetGuestAddress(), buffer_ptr,
                              buffer_length, unknown_ptr);
}

u32 XMsgStartIORequest_entry(u32 app, u32 message, GuestPtrT<kernel::XOVERLAPPED> overlapped_ptr,
                             u32 buffer_ptr, u32 buffer_length) {
  return XMsgStartIORequestEx(app, message, overlapped_ptr.GetGuestAddress(), buffer_ptr,
                              buffer_length, nullptr);
}

u32 XMsgCancelIORequest_entry(GuestPtrT<kernel::XOVERLAPPED> overlapped_ptr, u32 wait) {
  kernel::X_HANDLE event_handle = kernel::XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    CurrentTitle().threads.GetObjectWaiter().WaitGuestObject(event_handle, 0, 0, true, nullptr);
  }

  return 0;
}

u32 XMsgCompleteIORequest_entry(GuestPtrT<kernel::XOVERLAPPED> overlapped_ptr, u32 result,
                                u32 extended_error, u32 length) {
  CurrentTitle().overlapped_dispatcher.Complete(
      overlapped_ptr.GetGuestAddress(),
      {.result = result, .extended_error = extended_error, .length = length});
  return kernel::X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResult_entry(GuestPtrT<kernel::XOVERLAPPED> overlapped_ptr,
                                 MappedU32 length_ptr, [[maybe_unused]] u32 unknown) {
  kernel::XOVERLAPPED* overlapped = overlapped_ptr.GetHostPtr();
  auto read_result = [overlapped] {
    return CurrentTitle().overlapped_dispatcher.Observe(
        [&] { return kernel::XOverlappedGetResult(overlapped); });
  };
  u32 result = read_result();
  if (result == kernel::X_ERROR_IO_PENDING && !overlapped->event) {
    result = kernel::X_ERROR_IO_INCOMPLETE;
  } else if (result == kernel::X_ERROR_IO_PENDING) {
    result = CurrentTitle().threads.GetObjectWaiter().WaitGuestObject(overlapped->event, 3, 1, 0,
                                                                      nullptr);
    result = kernel::XSUCCEEDED(result) ? read_result()
                                        : kernel::xboxkrnl::RtlNtStatusToDosError(result);
  }
  if (kernel::XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped->length;
  }
  return result;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XMsgInProcessCall, rex::xam::XMsgInProcessCall_entry)
REX_XAM_BODY(__imp__XMsgSystemProcessCall, rex::xam::XMsgSystemProcessCall_entry)
REX_XAM_BODY(__imp__XMsgStartIORequestEx, rex::xam::XMsgStartIORequestEx_entry)
REX_XAM_BODY(__imp__XMsgStartIORequest, rex::xam::XMsgStartIORequest_entry)
REX_XAM_BODY(__imp__XMsgCancelIORequest, rex::xam::XMsgCancelIORequest_entry)
REX_XAM_BODY(__imp__XMsgCompleteIORequest, rex::xam::XMsgCompleteIORequest_entry)
REX_XAM_BODY(__imp__XamGetOverlappedResult, rex::xam::XamGetOverlappedResult_entry)
