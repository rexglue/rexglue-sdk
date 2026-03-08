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

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

ppc_u32_result_t XUsbcamCreate_entry(ppc_u32_t buffer,
                                     ppc_u32_t buffer_size,  // 0x4B000 640x480?
                                     ppc_pvoid_t unk3_ptr) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t XUsbcamGetState_entry() {
  // 0 = not connected.
  return 0;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__XUsbcamCreate, rex::kernel::xboxkrnl::XUsbcamCreate_entry)
XBOXKRNL_EXPORT(__imp__XUsbcamGetState, rex::kernel::xboxkrnl::XUsbcamGetState_entry)

XBOXKRNL_EXPORT_STUB(__imp__XUsbcamSetCaptureMode);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamGetConfig);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamSetConfig);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamReadFrame);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamSnapshot);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamSetView);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamGetView);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamDestroy);
XBOXKRNL_EXPORT_STUB(__imp__XUsbcamReset);
