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

PPC_HOOK(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
PPC_HOOK(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
PPC_HOOK(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
PPC_HOOK(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)
