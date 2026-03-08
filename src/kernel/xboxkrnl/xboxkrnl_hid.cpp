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

ppc_u32_result_t HidReadKeys_entry(ppc_u32_t unk1, ppc_unknown_t unk2, ppc_unknown_t unk3) {
  /* TODO(gibbed):
   * Games check for the following errors:
   *   0xC000009D - translated to 0x48F  - ERROR_DEVICE_NOT_CONNECTED
   *   0x103      - translated to 0x10D2 - ERROR_EMPTY
   * Other errors appear to be ignored?
   *
   * unk1 is 0
   * unk2 is a pointer to &unk3[2], possibly a 6-byte buffer
   * unk3 is a pointer to a 20-byte buffer
   */
  return 0xC000009D;
}

ppc_u32_result_t HidGetCapabilities_entry(ppc_u32_t unk1, ppc_unknown_t caps_ptr) {
  // HidGetCapabilities - ordinal 0x01EA
  // Returns capabilities for HID device (keyboard/mouse)
  // Not supported in rexglue - return unsuccessful
  return X_STATUS_UNSUCCESSFUL;
}

ppc_u32_result_t HidGetLastInputTime_entry(ppc_pu32_t time_ptr) {
  // HidGetLastInputTime - ordinal 0x01F1
  // Returns the last time any HID input was received
  if (time_ptr) {
    *time_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t HidReadMouseChanges_entry(ppc_u32_t unk1, ppc_unknown_t unk2) {
  // HidReadMouseChanges - ordinal 0x0273
  // Reads mouse input changes - not supported in rexglue
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
XBOXKRNL_EXPORT(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
XBOXKRNL_EXPORT(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
XBOXKRNL_EXPORT(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)

// XInputd stubs
XBOXKRNL_EXPORT_STUB(__imp__XInputdGetCapabilities);
XBOXKRNL_EXPORT_STUB(__imp__XInputdReadState);
XBOXKRNL_EXPORT_STUB(__imp__XInputdWriteState);
XBOXKRNL_EXPORT_STUB(__imp__XInputdNotify);
XBOXKRNL_EXPORT_STUB(__imp__XInputdRawState);
XBOXKRNL_EXPORT_STUB(__imp__XInputdGetDeviceStats);
XBOXKRNL_EXPORT_STUB(__imp__XInputdResetDevice);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetRingOfLight);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetRFPowerMode);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetRadioFrequency);
XBOXKRNL_EXPORT_STUB(__imp__XInputdPassThroughRFCommand);
XBOXKRNL_EXPORT_STUB(__imp__XInputdPowerDownDevice);
XBOXKRNL_EXPORT_STUB(__imp__XInputdReadTextKeystroke);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSendStayAliveRequest);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFGetDeviceInfo);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFSetEffect);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFUpdateEffect);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFEffectOperation);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFDeviceControl);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFSetDeviceGain);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFCancelIo);
XBOXKRNL_EXPORT_STUB(__imp__XInputdFFSetRumble);
XBOXKRNL_EXPORT_STUB(__imp__XInputdGetLastTextInputTime);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetTextMessengerIndicator);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetTextDeviceKeyLocks);
XBOXKRNL_EXPORT_STUB(__imp__XInputdGetTextDeviceKeyLocks);
XBOXKRNL_EXPORT_STUB(__imp__XInputdControl);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetWifiChannel);
XBOXKRNL_EXPORT_STUB(__imp__XInputdGetDevicePid);
XBOXKRNL_EXPORT_STUB(__imp__XInputdGetFailedConnectionOrBind);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetFailedConnectionOrBindCallback);
XBOXKRNL_EXPORT_STUB(__imp__XInputdSetMinMaxAuthDelay);

// Drv stubs
XBOXKRNL_EXPORT_STUB(__imp__DrvSetSysReqCallback);
XBOXKRNL_EXPORT_STUB(__imp__DrvSetUserBindingCallback);
XBOXKRNL_EXPORT_STUB(__imp__DrvSetContentStorageCallback);
XBOXKRNL_EXPORT_STUB(__imp__DrvSetAutobind);
XBOXKRNL_EXPORT_STUB(__imp__DrvGetContentStorageNotification);
XBOXKRNL_EXPORT_STUB(__imp__DrvXenonButtonPressed);
XBOXKRNL_EXPORT_STUB(__imp__DrvBindToUser);
XBOXKRNL_EXPORT_STUB(__imp__DrvSetDeviceConfigChangeCallback);
XBOXKRNL_EXPORT_STUB(__imp__DrvDeviceConfigChange);
XBOXKRNL_EXPORT_STUB(__imp__DrvSetMicArrayStartCallback);
XBOXKRNL_EXPORT_STUB(__imp__DrvSetAudioLatencyCallback);
