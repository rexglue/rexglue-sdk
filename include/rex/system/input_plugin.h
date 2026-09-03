/**
 * @file        system/input_plugin.h
 * @brief       Input plugin ABI and host-side loader
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on rex/system/gpu_plugin.h. Main plugin is rexinput_xsb.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <rex/system/interfaces/input.h>

#if defined(_WIN32)
#define REX_INPUT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REX_INPUT_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace rex::system {

// Bump on any change to InputCreateInfo or to the IInputSystem interface.
inline constexpr uint32_t kInputPluginAbiVersion = 2;

inline constexpr const char* kInputCreateSymbol = "rex_input_create";
inline constexpr const char* kInputAbiVersionSymbol = "rex_input_abi_version";

struct InputCreateInfo {
  uint32_t struct_size = 0;  // sizeof(InputCreateInfo), set by the host
  bool tool_mode = false;    // no window, so only the NOP driver is wanted
  InputAssignmentPolicy assignment = InputAssignmentPolicy::kPerUser;
};

// extern "C" exports every input plugin must provide:
//   uint32_t rex_input_abi_version(void);
//   rex::system::IInputSystem* rex_input_create(uint32_t abi_version,
//                                               const InputCreateInfo* info);
using InputAbiVersionFn = uint32_t (*)();
using InputCreateFn = IInputSystem* (*)(uint32_t abi_version, const InputCreateInfo* info);

// Plugin Loader
std::unique_ptr<IInputSystem> LoadInputPlugin(
    std::string_view name, bool tool_mode,
    InputAssignmentPolicy assignment = InputAssignmentPolicy::kPerUser);

}  // namespace rex::system
