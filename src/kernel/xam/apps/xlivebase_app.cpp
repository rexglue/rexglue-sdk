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

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXLOG_DEBUG(kernel, "XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXLOG_DEBUG(kernel, "XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXLOG_DEBUG(kernel, "CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr,
                   buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REX_KERNEL_STUB("CXLiveFriends::Enumerate({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REX_KERNEL_STUB("CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X})",
                      buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
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
