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
#include <rex/runtime/config.h>
#include <rex/runtime.h>
#include <rex/runtime/xstatus.h>

namespace rex::kernel::xboxkrnl {

/**
 * Returns the title to firmware.
 *
 * @param routine The reentry routine, must be 1 (HalRebootRoutine).
 */
void HalReturnToFirmware_entry(u32 routine) {
  REX_ASSERT(routine == 1);
  REXLOG_INFO(kernel, "Title requested a return to firmware");
  runtime::GetCurrent().ExitTitle(rex::runtime::TitleExitReason::kHardExit);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HalReturnToFirmware, rex::kernel::xboxkrnl::HalReturnToFirmware_entry)

REX_EXPORT_STUB(__imp__HalGetCurrentAVPack);
REX_EXPORT_STUB(__imp__HalGpioControl);
REX_EXPORT_STUB(__imp__HalOpenCloseODDTray);
REX_EXPORT_STUB(__imp__HalReadWritePCISpace);
REX_EXPORT_STUB(__imp__HalRegisterPowerDownNotification);
REX_EXPORT_STUB(__imp__HalRegisterSMCNotification);
REX_EXPORT_STUB(__imp__HalSendSMCMessage);
REX_EXPORT_STUB(__imp__HalSetAudioEnable);
REX_EXPORT_STUB(__imp__HalIsExecutingPowerDownDpc);
REX_EXPORT_STUB(__imp__HalGetPowerUpCause);
REX_EXPORT_STUB(__imp__HalRegisterPowerDownCallback);
REX_EXPORT_STUB(__imp__HalRegisterBackgroundModeTransitionCallback);
REX_EXPORT_STUB(__imp__HalClampUnclampOutputDACs);
REX_EXPORT_STUB(__imp__HalPowerDownToBackgroundMode);
REX_EXPORT_STUB(__imp__HalNotifyAddRemoveBackgroundTask);
REX_EXPORT_STUB(__imp__HalCallBackgroundModeNotificationRoutines);
REX_EXPORT_STUB(__imp__HalGetMemoryInformation);
REX_EXPORT_STUB(__imp__HalNotifyBackgroundModeTransitionComplete);
REX_EXPORT_STUB(__imp__HalFinalizePowerLossRecovery);
REX_EXPORT_STUB(__imp__HalSetPowerLossRecovery);
REX_EXPORT_STUB(__imp__HalRegisterXamPowerDownCallback);
REX_EXPORT_STUB(__imp__HalRegisterHdDvdRomNotification);
REX_EXPORT_STUB(__imp__HalGetNotedArgonErrors);
REX_EXPORT_STUB(__imp__HalReadArgonEeprom);
REX_EXPORT_STUB(__imp__HalWriteArgonEeprom);
REX_EXPORT_STUB(__imp__HalConfigureVeDevice);
