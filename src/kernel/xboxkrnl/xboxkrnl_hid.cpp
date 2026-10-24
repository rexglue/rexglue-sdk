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
#include <rex/runtime/xstatus.h>

namespace rex::kernel::xboxkrnl {

/**
 * Reads keys from a keyboard HID device. Reports the device as not connected.
 *
 * Titles check for 0xC000009D (translated to 0x48F ERROR_DEVICE_NOT_CONNECTED) and 0x103
 * (translated to 0x10D2 ERROR_EMPTY).
 *
 * @param unk1 Always 0.
 * @param unk2 Pointer to unk3 + 2.
 * @param unk3 Pointer to a 20 byte buffer.
 * @return 0xC000009D.
 */
u32 HidReadKeys_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                      [[maybe_unused]] u32 unk3) {
  return 0xC000009D;
}

/**
 * Reports the capabilities of a keyboard or mouse HID device. Unsupported.
 *
 * @param unk1 Unused.
 * @param caps_ptr Unused.
 * @return X_STATUS_UNSUCCESSFUL.
 */
u32 HidGetCapabilities_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 caps_ptr) {
  return X_STATUS_UNSUCCESSFUL;
}

/**
 * Returns the time of the last HID input, always 0.
 *
 * @param time_ptr Receives the time.
 * @return X_STATUS_SUCCESS.
 */
u32 HidGetLastInputTime_entry(MappedU32 time_ptr) {
  if (time_ptr) {
    *time_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}

/**
 * Reads mouse input changes. Unsupported.
 *
 * @param unk1 Unused.
 * @param unk2 Unused.
 * @return X_STATUS_UNSUCCESSFUL.
 */
u32 HidReadMouseChanges_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2) {
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
REX_EXPORT(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
REX_EXPORT(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
REX_EXPORT(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)

REX_EXPORT_STUB(__imp__XInputdGetCapabilities);
REX_EXPORT_STUB(__imp__XInputdReadState);
REX_EXPORT_STUB(__imp__XInputdWriteState);
REX_EXPORT_STUB(__imp__XInputdNotify);
REX_EXPORT_STUB(__imp__XInputdRawState);
REX_EXPORT_STUB(__imp__XInputdGetDeviceStats);
REX_EXPORT_STUB(__imp__XInputdResetDevice);
REX_EXPORT_STUB(__imp__XInputdSetRingOfLight);
REX_EXPORT_STUB(__imp__XInputdSetRFPowerMode);
REX_EXPORT_STUB(__imp__XInputdSetRadioFrequency);
REX_EXPORT_STUB(__imp__XInputdPassThroughRFCommand);
REX_EXPORT_STUB(__imp__XInputdPowerDownDevice);
REX_EXPORT_STUB(__imp__XInputdReadTextKeystroke);
REX_EXPORT_STUB(__imp__XInputdSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XInputdFFGetDeviceInfo);
REX_EXPORT_STUB(__imp__XInputdFFSetEffect);
REX_EXPORT_STUB(__imp__XInputdFFUpdateEffect);
REX_EXPORT_STUB(__imp__XInputdFFEffectOperation);
REX_EXPORT_STUB(__imp__XInputdFFDeviceControl);
REX_EXPORT_STUB(__imp__XInputdFFSetDeviceGain);
REX_EXPORT_STUB(__imp__XInputdFFCancelIo);
REX_EXPORT_STUB(__imp__XInputdFFSetRumble);
REX_EXPORT_STUB(__imp__XInputdGetLastTextInputTime);
REX_EXPORT_STUB(__imp__XInputdSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XInputdSetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdGetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdControl);
REX_EXPORT_STUB(__imp__XInputdSetWifiChannel);
REX_EXPORT_STUB(__imp__XInputdGetDevicePid);
REX_EXPORT_STUB(__imp__XInputdGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XInputdSetFailedConnectionOrBindCallback);
REX_EXPORT_STUB(__imp__XInputdSetMinMaxAuthDelay);

REX_EXPORT_STUB(__imp__DrvSetSysReqCallback);
REX_EXPORT_STUB(__imp__DrvSetUserBindingCallback);
REX_EXPORT_STUB(__imp__DrvSetContentStorageCallback);
REX_EXPORT_STUB(__imp__DrvSetAutobind);
REX_EXPORT_STUB(__imp__DrvGetContentStorageNotification);
REX_EXPORT_STUB(__imp__DrvXenonButtonPressed);
REX_EXPORT_STUB(__imp__DrvBindToUser);
REX_EXPORT_STUB(__imp__DrvSetDeviceConfigChangeCallback);
REX_EXPORT_STUB(__imp__DrvDeviceConfigChange);
REX_EXPORT_STUB(__imp__DrvSetMicArrayStartCallback);
REX_EXPORT_STUB(__imp__DrvSetAudioLatencyCallback);
