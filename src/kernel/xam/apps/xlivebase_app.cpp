/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/platform/thread.h>
#include "kernel/kernel_macros.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;
namespace apps {
using namespace rex::runtime;

XLiveBaseApp::XLiveBaseApp(Runtime& runtime) : App(runtime, 0xFC) {}

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REXLOG_DEBUG(kernel, "XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store<be_u32>(buffer + 0, 1);
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REXLOG_DEBUG(kernel, "XLiveBaseGetNatType({:08X})", buffer_ptr);
      // XONLINE_NAT_OPEN
      memory::store<be_u32>(buffer + 0, 1);
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // The buffer holds dwServiceId and a pServiceInfo pointer to XONLINE_SERVICE_INFO.
      REXLOG_DEBUG(kernel, "CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr,
                   buffer_length);
      // ERROR_CONNECTION_INVALID
      return 0x80151802;
    }
    case 0x00058020: {
      REX_KERNEL_STUB("CXLiveFriends::Enumerate({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REX_KERNEL_STUB("CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X})",
                      buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      REX_KERNEL_STUB("XLiveBaseUnk58046({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXLOG_DEBUG(kernel, "XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REX_FAIL(kernel, X_E_FAIL,
           "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
           "arg2={:08X}",
           app_id(), message, buffer_ptr, buffer_length);
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
