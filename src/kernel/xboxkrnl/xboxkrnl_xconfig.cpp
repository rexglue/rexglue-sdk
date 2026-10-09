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

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

namespace rex::kernel::xboxkrnl {

/**
 * Fills the value of a console configuration setting.
 *
 * Category 0x0002 (secured) supports setting 0x0002 (AV region), reported as 0x1000 (USA/Canada).
 * Category 0x0003 (user) supports the settings
 * - 0x0001 to 0x0007 (time zone), reported as 0
 * - 0x0009 (language)
 * - 0x000A (video flags)
 * - 0x000B (audio flags)
 * - 0x000C (retail flags)
 * - 0x000E (country)
 * - 0x0019 (PC flags), reported as 0x03 (XBLAllowed | XBLMembershipCreationAllowed)
 *
 * @param category The setting category.
 * @param setting The setting within the category.
 * @param buffer Receives the value, may be null to query the size.
 * @param buffer_size The size of buffer in bytes.
 * @param required_size Receives the size of the value in bytes, may be null.
 * @return X_STATUS_SUCCESS or an error status.
 */
X_STATUS ExGetXConfigSetting(uint16_t category, uint16_t setting, void* buffer,
                             uint16_t buffer_size, uint16_t* required_size) {
  uint16_t setting_size = 0;
  alignas(uint32_t) uint8_t value[4];

  switch (category) {
    case 0x0002:
      switch (setting) {
        case 0x0002:
          setting_size = 4;
          memory::store<be_u32>(value, 0x00001000);
          break;
        default:
          REX_KERNEL_STUB("XConfig SECURED setting 0x{:04X}", setting);
          return X_STATUS_INVALID_PARAMETER_2;
      }
      break;
    case 0x0003:
      switch (setting) {
        case 0x0001:
        case 0x0002:
        case 0x0003:
        case 0x0004:
        case 0x0005:
        case 0x0006:
        case 0x0007:
          setting_size = 4;
          memory::store<be_u32>(value, 0);
          break;
        case 0x0009:
          setting_size = 4;
          memory::store<be_u32>(value, runtime::Current().config().runtime.kernel.user_language);
          break;
        case 0x000A:
          setting_size = 4;
          memory::store<be_u32>(value, 0x00040000);
          break;
        case 0x000B:
          setting_size = 4;
          memory::store<be_u32>(value, 0x00010001);
          break;
        case 0x000C:
          setting_size = 4;
          memory::store<be_u32>(value, 0x40);
          break;
        case 0x000E:
          setting_size = 1;
          value[0] = static_cast<uint8_t>(runtime::Current().config().runtime.kernel.user_country);
          break;
        case 0x0019:
          setting_size = 1;
          value[0] = 0x03;
          break;
        default:
          REX_KERNEL_STUB("XConfig USER setting 0x{:04X}", setting);
          return X_STATUS_INVALID_PARAMETER_2;
      }
      break;
    default:
      REX_KERNEL_STUB("XConfig category 0x{:04X}", category);
      return X_STATUS_INVALID_PARAMETER_1;
  }

  if (buffer) {
    if (buffer_size < setting_size) {
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, value, setting_size);
  } else {
    if (buffer_size) {
      return X_STATUS_INVALID_PARAMETER_3;
    }
  }

  if (required_size) {
    *required_size = setting_size;
  }

  return X_STATUS_SUCCESS;
}

u32 ExGetXConfigSetting_entry(u16 category, u16 setting, mapped_void buffer_ptr, u16 buffer_size,
                              mapped_u16 required_size_ptr) {
  uint16_t required_size = 0;
  X_STATUS result = ExGetXConfigSetting(category, setting, buffer_ptr, buffer_size, &required_size);

  if (required_size_ptr) {
    *required_size_ptr = required_size;
  }

  return result;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__ExGetXConfigSetting, rex::kernel::xboxkrnl::ExGetXConfigSetting_entry)
REX_EXPORT_STUB(__imp__ExSetXConfigSetting);
