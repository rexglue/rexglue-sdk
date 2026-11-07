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

#include <rex/ppc/hook.h>
#include <rex/hid/input_system.h>
#include <rex/xam/input.h>
#include "xam/xam_ui.h"
#include <rex/logging/log_category.h>
#include "xam/categories.h"
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/types.h>

#include "xam/input_convert.h"
#include <rex/kernel/thread_state.h>
#include "xam/xam_state.h"
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

using rex::xam::X_INPUT_CAPABILITIES;
using rex::xam::X_INPUT_KEYSTROKE;
using rex::xam::X_INPUT_STATE;
using rex::xam::X_INPUT_VIBRATION;

namespace {

rex::hid::IInputSystem* GetInput() {
  return CurrentXAM().GetInput();
}

u32 NormalizeUser(u32 user_index, u32 flags) {
  if ((user_index & 0xFF) == 0xFF || (flags & rex::xam::kXInputFlagAnyUser)) {
    return 0;
  }
  return user_index;
}

}  // namespace

void XamResetInactivity_entry() {}

u32 XamEnableInactivityProcessing_entry([[maybe_unused]] u32 unk, [[maybe_unused]] u32 enable) {
  return kernel::X_ERROR_SUCCESS;
}

u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, GuestPtrT<X_INPUT_CAPABILITIES> caps) {
  REXXAM_TRACE("XAM XamInputGetCapabilities called: user={}, flags=0x{:X}",
               static_cast<u32>(user_index), static_cast<u32>(flags));
  if (!caps) {
    return kernel::X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & rex::xam::kXInputFlagGamepad) == 0) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const u32 actual_user_index = NormalizeUser(user_index, flags);
  rex::hid::IInputSystem* input = GetInput();
  if (!input || actual_user_index >= rex::hid::IInputSystem::kMaxUsers) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto result = input->GetCapabilities(actual_user_index);
  if (!result) {
    return ToInputResult(result.error());
  }
  X_INPUT_CAPABILITIES* out = caps;
  *out = ToXInputCapabilities(*result);
  return kernel::X_ERROR_SUCCESS;
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    GuestPtrT<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return kernel::X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & rex::xam::kXInputFlagGamepad) == 0) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }

  (void)unk;
  const u32 actual_user_index = NormalizeUser(user_index, flags);
  rex::hid::IInputSystem* input = GetInput();
  if (!input || actual_user_index >= rex::hid::IInputSystem::kMaxUsers) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto result = input->GetCapabilities(actual_user_index);
  if (!result) {
    return ToInputResult(result.error());
  }
  X_INPUT_CAPABILITIES* out = caps;
  *out = ToXInputCapabilities(*result);
  return kernel::X_ERROR_SUCCESS;
}

u32 XamInputGetState_entry(u32 user_index, u32 flags, GuestPtrT<X_INPUT_STATE> input_state) {
  if (CurrentXAM().CountInputStateCall() <= 5) {
    REXXAM_TRACE("XAM XamInputGetState called: user={}, flags=0x{:X}", static_cast<u32>(user_index),
                 static_cast<u32>(flags));
  }

  if ((flags & 0xFF) && (flags & rex::xam::kXInputFlagGamepad) == 0) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const u32 actual_user_index = NormalizeUser(user_index, flags);
  rex::hid::IInputSystem* input = GetInput();
  if (!input || actual_user_index >= rex::hid::IInputSystem::kMaxUsers) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto result = input->GetState(actual_user_index);
  if (!result) {
    return ToInputResult(result.error());
  }
  if (input_state) {
    X_INPUT_STATE* out = input_state;
    *out = ToXInputState(*result);
  }
  return kernel::X_ERROR_SUCCESS;
}

u32 XamInputSetState_entry(u32 user_index, u32 unk, GuestPtrT<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return kernel::X_ERROR_BAD_ARGUMENTS;
  }

  u32 actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    actual_user_index = 0;
  }

  (void)unk;
  rex::hid::IInputSystem* input = GetInput();
  if (!input || actual_user_index >= rex::hid::IInputSystem::kMaxUsers) {
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const X_INPUT_VIBRATION* requested = vibration;
  auto result = input->SetVibration(actual_user_index, ToPadVibration(*requested));
  return result ? kernel::X_ERROR_SUCCESS : ToInputResult(result.error());
}

namespace {

u32 GetKeystroke(u32& user, u32 flags, GuestPtrT<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return kernel::X_ERROR_BAD_ARGUMENTS;
  }
  X_INPUT_KEYSTROKE* out = keystroke;
  rex::hid::IInputSystem* input = GetInput();
  if (!input) {
    *out = {};
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return TakeInputKeystroke(*input, user, flags, *out);
}

}  // namespace

u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, GuestPtrT<X_INPUT_KEYSTROKE> keystroke) {
  return GetKeystroke(user_index, flags, keystroke);
}

u32 XamInputGetKeystrokeEx_entry(MappedU32 user_index_ptr, u32 flags,
                                 GuestPtrT<X_INPUT_KEYSTROKE> keystroke) {
  u32 user = *user_index_ptr;
  const u32 result = GetKeystroke(user, flags, keystroke);
  if (result == kernel::X_ERROR_SUCCESS && (flags & rex::xam::kXInputFlagAnyUser)) {
    *user_index_ptr = user;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, [[maybe_unused]] u32 unk, MappedU32 out_ptr) {
  // Titles read the output as zero when the call fails.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return kernel::X_E_SUCCESS;
  } else {
    return static_cast<i32>(kernel::X_E_DEVICE_NOT_CONNECTED);
  }
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamResetInactivity, rex::xam::XamResetInactivity_entry)
REX_XAM_BODY(__imp__XamEnableInactivityProcessing, rex::xam::XamEnableInactivityProcessing_entry)
REX_XAM_BODY(__imp__XamInputGetCapabilities, rex::xam::XamInputGetCapabilities_entry)
REX_XAM_BODY(__imp__XamInputGetCapabilitiesEx, rex::xam::XamInputGetCapabilitiesEx_entry)
REX_XAM_BODY(__imp__XamInputGetState, rex::xam::XamInputGetState_entry)
REX_XAM_BODY(__imp__XamInputSetState, rex::xam::XamInputSetState_entry)
REX_XAM_BODY(__imp__XamInputGetKeystroke, rex::xam::XamInputGetKeystroke_entry)
REX_XAM_BODY(__imp__XamInputGetKeystrokeEx, rex::xam::XamInputGetKeystrokeEx_entry)
REX_XAM_BODY(__imp__XamUserGetDeviceContext, rex::xam::XamUserGetDeviceContext_entry)
