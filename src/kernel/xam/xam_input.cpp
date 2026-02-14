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

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/kernel_state.h>
#include <rex/runtime/guest/function.h>
#include <rex/runtime/guest/types.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime::guest;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

dword_result_t XamEnableInactivityProcessing_entry(dword_t unk,
                                                   dword_t enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
dword_result_t XamInputGetCapabilities_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_DEBUG("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index, (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->input_system();
  return input_system->GetCapabilities(actual_user_index, flags, caps);
}

dword_result_t XamInputGetCapabilitiesEx_entry(
    dword_t unk, dword_t user_index, dword_t flags,
    pointer_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto input_system = kernel_state()->input_system();
  return input_system->GetCapabilities(actual_user_index, flags, caps);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
dword_result_t XamInputGetState_entry(dword_t user_index, dword_t flags,
                                      pointer_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_DEBUG("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index, (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->input_system();
  return input_system->GetState(actual_user_index, input_state);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
dword_result_t XamInputSetState_entry(dword_t user_index, dword_t unk,
                                      pointer_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto input_system = kernel_state()->input_system();
  return input_system->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
dword_result_t XamInputGetKeystroke_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->input_system();
  return input_system->GetKeystroke(actual_user_index, flags, keystroke);
}

// Same as non-ex, just takes a pointer to user index.
dword_result_t XamInputGetKeystrokeEx_entry(
    lpdword_t user_index_ptr, dword_t flags,
    pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto input_system = kernel_state()->input_system();
  auto result = input_system->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

X_HRESULT_result_t XamUserGetDeviceContext_entry(dword_t user_index,
                                                 dword_t unk,
                                                 lpdword_t out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

GUEST_FUNCTION_HOOK(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
GUEST_FUNCTION_HOOK(__imp__XamEnableInactivityProcessing, rex::kernel::xam::XamEnableInactivityProcessing_entry)
GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
GUEST_FUNCTION_HOOK(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
GUEST_FUNCTION_HOOK(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
GUEST_FUNCTION_HOOK(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
GUEST_FUNCTION_HOOK(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
GUEST_FUNCTION_HOOK(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)
