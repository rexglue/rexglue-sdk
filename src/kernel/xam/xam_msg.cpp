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
#include <rex/logging.h>
#include <rex/hook.h>
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
  auto result = runtime::Current().content_manager().app_manager().DispatchMessageSync(app, message,
                                                                                       arg1, arg2);
  if (result == X_ERROR_NOT_FOUND) {
    REXLOG_ERROR(kernel, "XMsgInProcessCall: app {:08X} undefined", app);
  }
  return result;
}

u32 XMsgSystemProcessCall_entry(u32 app, u32 message, u32 buffer, u32 buffer_length) {
  auto result = runtime::Current().content_manager().app_manager().DispatchMessageAsync(
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
  auto result = runtime::Current().content_manager().app_manager().DispatchMessageAsync(
      app, message, buffer_ptr, buffer_length);
  if (result == X_E_NOTFOUND) {
    REXLOG_ERROR(kernel, "XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    KernelThread::SetLastError(X_ERROR_NOT_FOUND);
  }
  if (overlapped_ptr) {
    runtime::Current().dispatcher().CompleteOverlappedImmediate(overlapped_ptr, result);
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    KernelThread::SetLastError(0);
  }
  return result;
}

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                               u32 buffer_ptr, u32 buffer_length,
                               ppc_ptr_t<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return XMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                              buffer_length, unknown_ptr);
}

u32 XMsgStartIORequest_entry(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                             u32 buffer_ptr, u32 buffer_length) {
  return XMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                              buffer_length, nullptr);
}

u32 XMsgCancelIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 wait) {
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    Wait(runtime::Current(), event_handle, 0, 0, true, nullptr);
  }

  return 0;
}

u32 XMsgCompleteIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 result,
                                u32 extended_error, u32 length) {
  runtime::Current().dispatcher().CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(),
                                                                result, extended_error, length);
  return X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResult_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, mapped_u32 length_ptr,
                                 [[maybe_unused]] u32 unknown) {
  XAM_OVERLAPPED* overlapped = overlapped_ptr.host_address();
  u32 result = XOverlappedGetResult(overlapped);
  if (result == X_ERROR_IO_PENDING && !overlapped->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else if (result == X_ERROR_IO_PENDING) {
    result = Wait(runtime::Current(), overlapped->event, 3, 1, 0, nullptr);
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
