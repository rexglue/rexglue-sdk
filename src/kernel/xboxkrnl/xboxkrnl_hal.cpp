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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void HalReturnToFirmware_entry(ppc_u32_t routine) {
  // void
  // IN FIRMWARE_REENTRY  Routine

  // Routine must be 1 'HalRebootRoutine'
  assert_true(routine == 1);

  // TODO(benvank): diediedie much more gracefully
  // Not sure how to blast back up the stack in LLVM without exceptions, though.
  REXKRNL_ERROR("Game requested shutdown via HalReturnToFirmware");
  exit(0);
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__HalReturnToFirmware, rex::kernel::xboxkrnl::HalReturnToFirmware_entry)

XBOXKRNL_EXPORT_STUB(__imp__HalGetCurrentAVPack);
XBOXKRNL_EXPORT_STUB(__imp__HalGpioControl);
XBOXKRNL_EXPORT_STUB(__imp__HalOpenCloseODDTray);
XBOXKRNL_EXPORT_STUB(__imp__HalReadWritePCISpace);
XBOXKRNL_EXPORT_STUB(__imp__HalRegisterPowerDownNotification);
XBOXKRNL_EXPORT_STUB(__imp__HalRegisterSMCNotification);
XBOXKRNL_EXPORT_STUB(__imp__HalSendSMCMessage);
XBOXKRNL_EXPORT_STUB(__imp__HalSetAudioEnable);
XBOXKRNL_EXPORT_STUB(__imp__HalIsExecutingPowerDownDpc);
XBOXKRNL_EXPORT_STUB(__imp__HalGetPowerUpCause);
XBOXKRNL_EXPORT_STUB(__imp__HalRegisterPowerDownCallback);
XBOXKRNL_EXPORT_STUB(__imp__HalRegisterBackgroundModeTransitionCallback);
XBOXKRNL_EXPORT_STUB(__imp__HalClampUnclampOutputDACs);
XBOXKRNL_EXPORT_STUB(__imp__HalPowerDownToBackgroundMode);
XBOXKRNL_EXPORT_STUB(__imp__HalNotifyAddRemoveBackgroundTask);
XBOXKRNL_EXPORT_STUB(__imp__HalCallBackgroundModeNotificationRoutines);
XBOXKRNL_EXPORT_STUB(__imp__HalGetMemoryInformation);
XBOXKRNL_EXPORT_STUB(__imp__HalNotifyBackgroundModeTransitionComplete);
XBOXKRNL_EXPORT_STUB(__imp__HalFinalizePowerLossRecovery);
XBOXKRNL_EXPORT_STUB(__imp__HalSetPowerLossRecovery);
XBOXKRNL_EXPORT_STUB(__imp__HalRegisterXamPowerDownCallback);
XBOXKRNL_EXPORT_STUB(__imp__HalRegisterHdDvdRomNotification);
XBOXKRNL_EXPORT_STUB(__imp__HalGetNotedArgonErrors);
XBOXKRNL_EXPORT_STUB(__imp__HalReadArgonEeprom);
XBOXKRNL_EXPORT_STUB(__imp__HalWriteArgonEeprom);
XBOXKRNL_EXPORT_STUB(__imp__HalConfigureVeDevice);
