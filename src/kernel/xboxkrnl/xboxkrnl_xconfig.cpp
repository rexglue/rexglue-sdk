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

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

X_STATUS xeExGetXConfigSetting(uint16_t category, uint16_t setting, void* buffer,
                               uint16_t buffer_size, uint16_t* required_size) {
  uint16_t setting_size = 0;
  alignas(uint32_t) uint8_t value[4];

  // TODO(benvanik): have real structs here that just get copied from.
  // https://free60project.github.io/wiki/XConfig.html
  // https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/ExConfig.h
  switch (category) {
    case 0x0002:
      // XCONFIG_SECURED_CATEGORY
      switch (setting) {
        case 0x0002:  // XCONFIG_SECURED_AV_REGION
          setting_size = 4;
          memory::store_and_swap<uint32_t>(value, 0x00001000);  // USA/Canada
          break;
        default:
          REXKRNL_WARN("Unimplemented XConfig SECURED setting 0x{:04X}", setting);
          return X_STATUS_INVALID_PARAMETER_2;
      }
      break;
    case 0x0003:
      // XCONFIG_USER_CATEGORY
      switch (setting) {
        case 0x0001:  // XCONFIG_USER_TIME_ZONE_BIAS
        case 0x0002:  // XCONFIG_USER_TIME_ZONE_STD_NAME
        case 0x0003:  // XCONFIG_USER_TIME_ZONE_DLT_NAME
        case 0x0004:  // XCONFIG_USER_TIME_ZONE_STD_DATE
        case 0x0005:  // XCONFIG_USER_TIME_ZONE_DLT_DATE
        case 0x0006:  // XCONFIG_USER_TIME_ZONE_STD_BIAS
        case 0x0007:  // XCONFIG_USER_TIME_ZONE_DLT_BIAS
          setting_size = 4;
          // TODO(benvanik): get this value.
          memory::store_and_swap<uint32_t>(value, 0);
          break;
        case 0x0009:  // XCONFIG_USER_LANGUAGE
          setting_size = 4;
          memory::store_and_swap<uint32_t>(value, REXCVAR_GET(user_language));
          break;
        case 0x000A:  // XCONFIG_USER_VIDEO_FLAGS
          setting_size = 4;
          memory::store_and_swap<uint32_t>(value, 0x00040000);
          break;
        case 0x000B:  // XCONFIG_USER_AUDIO_FLAGS
          setting_size = 4;
          memory::store_and_swap<uint32_t>(value, 0x00010001);
          break;
        case 0x000C:  // XCONFIG_USER_RETAIL_FLAGS
          setting_size = 4;
          memory::store_and_swap<uint32_t>(value, 0x40);
          break;
        case 0x000E:  // XCONFIG_USER_COUNTRY
          setting_size = 1;
          value[0] = static_cast<uint8_t>(REXCVAR_GET(user_country));
          break;
        case 0x0019:  // XCONFIG_USER_PC_FLAGS
          setting_size = 1;
          // XBLAllowed | XBLMembershipCreationAllowed
          value[0] = 0x03;
          break;
        default:
          REXKRNL_WARN("Unimplemented XConfig USER setting 0x{:04X}", setting);
          return X_STATUS_INVALID_PARAMETER_2;
      }
      break;
    default:
      REXKRNL_WARN("Unimplemented XConfig category 0x{:04X}", category);
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

ppc_u32_result_t ExGetXConfigSetting_entry(ppc_u16_t category, ppc_u16_t setting,
                                           ppc_pvoid_t buffer_ptr, ppc_u16_t buffer_size,
                                           ppc_pu16_t required_size_ptr) {
  uint16_t required_size = 0;
  X_STATUS result =
      xeExGetXConfigSetting(category, setting, buffer_ptr, buffer_size, &required_size);

  if (required_size_ptr) {
    *required_size_ptr = required_size;
  }

  return result;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__ExGetXConfigSetting, rex::kernel::xboxkrnl::ExGetXConfigSetting_entry)
XBOXKRNL_EXPORT_STUB(__imp__ExSetXConfigSetting);
