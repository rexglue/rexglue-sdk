/**
 * @file        input/plugin_main.cpp
 * @brief       rexinput_xsb plugin entry points
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <memory>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/input_plugin.h>

extern "C" REX_INPUT_PLUGIN_EXPORT uint32_t rex_input_abi_version(void) {
  return rex::system::kInputPluginAbiVersion;
}

extern "C" REX_INPUT_PLUGIN_EXPORT rex::system::IInputSystem* rex_input_create(
    uint32_t abi_version, const rex::system::InputCreateInfo* info) {
  if (abi_version != rex::system::kInputPluginAbiVersion) {
    REXLOG_ERROR("rexinput_xsb: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kInputPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::InputCreateInfo)) {
    REXLOG_ERROR("rexinput_xsb: invalid InputCreateInfo");
    return nullptr;
  }

  auto input = rex::input::CreateDefaultInputSystem(info->tool_mode);
  if (input && info->assignment == rex::system::InputAssignmentPolicy::kShared) {
    input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
  }
  return input.release();
}
