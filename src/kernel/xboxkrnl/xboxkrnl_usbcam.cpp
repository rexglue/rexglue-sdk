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

#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xtypes.h>

namespace rex::kernel::xboxkrnl {

/**
 * Reports success without creating a camera, because titles that see a failure skip their
 * initialization and then run on uninitialized data.
 *
 * @param buffer Unused.
 * @param buffer_size Unused.
 * @param unk3_ptr Unused.
 * @return X_STATUS_SUCCESS.
 */
u32 XUsbcamCreate_entry([[maybe_unused]] u32 buffer, [[maybe_unused]] u32 buffer_size,
                        [[maybe_unused]] MappedVoid unk3_ptr) {
  return X_STATUS_SUCCESS;
}

/**
 * Returns the camera state.
 *
 * @return 0, meaning not connected.
 */
u32 XUsbcamGetState_entry() {
  return 0;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XUsbcamCreate, rex::kernel::xboxkrnl::XUsbcamCreate_entry)
REX_EXPORT(__imp__XUsbcamGetState, rex::kernel::xboxkrnl::XUsbcamGetState_entry)

REX_EXPORT_STUB(__imp__XUsbcamSetCaptureMode);
REX_EXPORT_STUB(__imp__XUsbcamGetConfig);
REX_EXPORT_STUB(__imp__XUsbcamSetConfig);
REX_EXPORT_STUB(__imp__XUsbcamReadFrame);
REX_EXPORT_STUB(__imp__XUsbcamSnapshot);
REX_EXPORT_STUB(__imp__XUsbcamSetView);
REX_EXPORT_STUB(__imp__XUsbcamGetView);
REX_EXPORT_STUB(__imp__XUsbcamDestroy);
REX_EXPORT_STUB(__imp__XUsbcamReset);
