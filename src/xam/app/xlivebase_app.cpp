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

#include "xam/app/apps.h"
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/platform/thread.h>
#include <rex/xam/title_lifecycle.h>
#include "xam/stub_log.h"

namespace rex {
namespace xam {

using namespace rex::kernel;
namespace apps {
using namespace rex::kernel;

XLiveBaseApp::XLiveBaseApp(const XAMKernelInterfaces& title) : rex::xam::XAMApplet(title, 0xFC) {}

kernel::X_HRESULT XLiveBaseApp::DispatchMessageSync(u32 message, u32 buffer_ptr,
                                                    u32 buffer_length) {
  auto buffer = membase_.TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REXLOG_DEBUG(xam, "XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store<be_u32>(buffer + 0, 1);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058006: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REXLOG_DEBUG(xam, "XLiveBaseGetNatType({:08X})", buffer_ptr);
      // XONLINE_NAT_OPEN
      memory::store<be_u32>(buffer + 0, 1);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058007: {
      // The buffer holds dwServiceId and a pServiceInfo pointer to XONLINE_SERVICE_INFO.
      REXLOG_DEBUG(xam, "CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      // XONLINE_E_LOGON_NO_NETWORK_CONNECTION
      return 0x80151802;
    }
    case 0x00058020: {
      REX_XAM_STUB("CXLiveFriends::Enumerate({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_FAIL;
    }
    case 0x00058023: {
      REX_XAM_STUB("CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X})", buffer_ptr,
                   buffer_length);
      return kernel::X_E_FAIL;
    }
    case 0x00058046: {
      REX_XAM_STUB("XLiveBaseUnk58046({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058037: {
      REXLOG_DEBUG(xam, "XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_SUCCESS;
    }
    default:
      break;
  }
  REX_FAIL(xam, kernel::X_E_FAIL,
           "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
           "arg2={:08X}",
           GetAppID(), message, buffer_ptr, buffer_length);
}

}  // namespace apps

}  // namespace xam
}  // namespace rex
