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

#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/dispatcher.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  auto result = runtime::GetCurrent().GetContentManager().GetAppManager().DispatchMessageSync(
      app, message, arg1, arg2);
  if (result == X_ERROR_NOT_FOUND) {
    REXLOG_ERROR(kernel, "XMsgInProcessCall: app {:08X} undefined", app);
  }
  return result;
}

u32 XMsgSystemProcessCall_entry(u32 app, u32 message, u32 buffer, u32 buffer_length) {
  auto result = runtime::GetCurrent().GetContentManager().GetAppManager().DispatchMessageAsync(
      app, message, buffer, buffer_length);
  if (result == X_ERROR_NOT_FOUND) {
    REXLOG_ERROR(kernel, "XMsgSystemProcessCall: app {:08X} undefined", app);
  }
  return result;
}

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be_u32 unk_0;
  be_u32 unk_1;
};

X_HRESULT XMsgStartIORequestEx(u32 app, u32 message, u32 overlapped_ptr, u32 buffer_ptr,
                               u32 buffer_length,
                               [[maybe_unused]] XMSGSTARTIOREQUEST_UNKNOWNARG* unknown) {
  auto result = runtime::GetCurrent().GetContentManager().GetAppManager().DispatchMessageAsync(
      app, message, buffer_ptr, buffer_length);
  if (result == X_E_NOTFOUND) {
    REXLOG_ERROR(kernel, "XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    KernelThread::GetCurrentThread()->SetLastError(X_ERROR_NOT_FOUND);
  }
  if (overlapped_ptr) {
    runtime::GetCurrent().GetDispatcher().Complete(overlapped_ptr, {.result = result});
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    KernelThread::GetCurrentThread()->SetLastError(0);
  }
  return result;
}

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, GuestPtrT<XAM_OVERLAPPED> overlapped_ptr,
                               u32 buffer_ptr, u32 buffer_length,
                               GuestPtrT<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return XMsgStartIORequestEx(app, message, overlapped_ptr.GetGuestAddress(), buffer_ptr,
                              buffer_length, unknown_ptr);
}

u32 XMsgStartIORequest_entry(u32 app, u32 message, GuestPtrT<XAM_OVERLAPPED> overlapped_ptr,
                             u32 buffer_ptr, u32 buffer_length) {
  return XMsgStartIORequestEx(app, message, overlapped_ptr.GetGuestAddress(), buffer_ptr,
                              buffer_length, nullptr);
}

u32 XMsgCancelIORequest_entry(GuestPtrT<XAM_OVERLAPPED> overlapped_ptr, u32 wait) {
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    Wait(runtime::GetCurrent(), event_handle, 0, 0, true, nullptr);
  }

  return 0;
}

u32 XMsgCompleteIORequest_entry(GuestPtrT<XAM_OVERLAPPED> overlapped_ptr, u32 result,
                                u32 extended_error, u32 length) {
  runtime::GetCurrent().GetDispatcher().Complete(
      overlapped_ptr.GetGuestAddress(),
      {.result = result, .extended_error = extended_error, .length = length});
  return X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResult_entry(GuestPtrT<XAM_OVERLAPPED> overlapped_ptr, MappedU32 length_ptr,
                                 [[maybe_unused]] u32 unknown) {
  XAM_OVERLAPPED* overlapped = overlapped_ptr.GetHostAddress();
  u32 result = XOverlappedGetResult(overlapped);
  if (result == X_ERROR_IO_PENDING && !overlapped->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else if (result == X_ERROR_IO_PENDING) {
    result = Wait(runtime::GetCurrent(), overlapped->event, 3, 1, 0, nullptr);
    result = XSUCCEEDED(result) ? XOverlappedGetResult(overlapped)
                                : xboxkrnl::RtlNtStatusToDosError(result);
  }
  if (XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped->length;
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XMsgInProcessCall, rex::kernel::xam::XMsgInProcessCall_entry)
REX_EXPORT(__imp__XMsgSystemProcessCall, rex::kernel::xam::XMsgSystemProcessCall_entry)
REX_EXPORT(__imp__XMsgStartIORequestEx, rex::kernel::xam::XMsgStartIORequestEx_entry)
REX_EXPORT(__imp__XMsgStartIORequest, rex::kernel::xam::XMsgStartIORequest_entry)
REX_EXPORT(__imp__XMsgCancelIORequest, rex::kernel::xam::XMsgCancelIORequest_entry)
REX_EXPORT(__imp__XMsgCompleteIORequest, rex::kernel::xam::XMsgCompleteIORequest_entry)
REX_EXPORT(__imp__XamGetOverlappedResult, rex::kernel::xam::XamGetOverlappedResult_entry)

REX_EXPORT_STUB(__imp__XMsgAcquireAsyncMessageFromOverlapped);
REX_EXPORT_STUB(__imp__XMsgProcessRequest);
REX_EXPORT_STUB(__imp__XMsgReleaseAsyncMessageToOverlapped);
