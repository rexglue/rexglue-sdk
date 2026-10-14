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

#include <rex/hid.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

using rex::hid::X_INPUT_CAPABILITIES;
using rex::hid::X_INPUT_KEYSTROKE;
using rex::hid::X_INPUT_STATE;
using rex::hid::X_INPUT_VIBRATION;

rex::hid::IHIDBackend& HID() {
  return runtime::GetCurrent().GetHID();
}

X_RESULT Fail(const rex::Error& error) {
  return rex::hid::ToXResult(rex::hid::CodeOf(error));
}

void XamResetInactivity_entry() {}

u32 XamEnableInactivityProcessing_entry([[maybe_unused]] u32 unk, [[maybe_unused]] u32 enable) {
  return X_ERROR_SUCCESS;
}

u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, GuestPtrT<X_INPUT_CAPABILITIES> caps) {
  REXLOG_TRACE(kernel, "XAM XamInputGetCapabilities called: user={}, flags=0x{:X}",
               static_cast<u32>(user_index), static_cast<u32>(flags));
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & hid::kXInputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  u32 actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & hid::kXInputFlagAnyUser)) {
    actual_user_index = 0;
  }

  auto result = HID().GetCapabilities(actual_user_index, flags);
  if (!result) {
    return Fail(result.error());
  }
  X_INPUT_CAPABILITIES* out = caps;
  *out = *result;
  return X_ERROR_SUCCESS;
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    GuestPtrT<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & hid::kXInputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  u32 actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & hid::kXInputFlagAnyUser)) {
    actual_user_index = 0;
  }

  (void)unk;
  auto result = HID().GetCapabilities(actual_user_index, flags);
  if (!result) {
    return Fail(result.error());
  }
  X_INPUT_CAPABILITIES* out = caps;
  *out = *result;
  return X_ERROR_SUCCESS;
}

u32 XamInputGetState_entry(u32 user_index, u32 flags, GuestPtrT<X_INPUT_STATE> input_state) {
  static int call_count = 0;
  if (++call_count <= 5) {
    REXLOG_TRACE(kernel, "XAM XamInputGetState called: user={}, flags=0x{:X}",
                 static_cast<u32>(user_index), static_cast<u32>(flags));
  }

  if ((flags & 0xFF) && (flags & hid::kXInputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  u32 actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & hid::kXInputFlagAnyUser)) {
    actual_user_index = 0;
  }

  auto result = HID().GetState(actual_user_index);
  if (!result) {
    return Fail(result.error());
  }
  if (input_state) {
    X_INPUT_STATE* out = input_state;
    *out = *result;
  }
  return X_ERROR_SUCCESS;
}

u32 XamInputSetState_entry(u32 user_index, u32 unk, GuestPtrT<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  u32 actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    actual_user_index = 0;
  }

  (void)unk;
  auto result = HID().SetVibration(actual_user_index, *vibration);
  return result ? X_ERROR_SUCCESS : Fail(result.error());
}

u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, GuestPtrT<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & hid::kXInputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  u32 actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & hid::kXInputFlagAnyUser)) {
    actual_user_index = 0;
  }

  auto result = HID().GetKeystroke(actual_user_index, flags);
  if (!result) {
    return Fail(result.error());
  }
  X_INPUT_KEYSTROKE* out = keystroke;
  *out = *result;
  return X_ERROR_SUCCESS;
}

u32 XamInputGetKeystrokeEx_entry(MappedU32 user_index_ptr, u32 flags,
                                 GuestPtrT<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & hid::kXInputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  u32 user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & hid::kXInputFlagAnyUser)) {
    user_index = 0;
  }

  auto result = HID().GetKeystroke(user_index, flags);
  if (!result) {
    return Fail(result.error());
  }
  X_INPUT_KEYSTROKE* out = keystroke;
  *out = *result;
  *user_index_ptr = result->user_index;
  return X_ERROR_SUCCESS;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, [[maybe_unused]] u32 unk, MappedU32 out_ptr) {
  // Titles read the output as zero when the call fails.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return X_E_SUCCESS;
  } else {
    return static_cast<i32>(X_E_DEVICE_NOT_CONNECTED);
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
