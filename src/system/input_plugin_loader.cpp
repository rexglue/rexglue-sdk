/**
 * @file        system/input_plugin_loader.cpp
 * @brief       Host-side loader for input plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/input_plugin.h>

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {

namespace {

// Plugin binaries follow the SDK's per-config postfix convention; this TU is
// part of rexruntime, so REXGLUE_BUILD_CONFIG matches the plugin's config.
std::string PluginFileName(std::string_view name) {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix = "";
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }
#if REX_PLATFORM_WIN32
  return fmt::format("rexinput_{}{}.dll", name, postfix);
#elif REX_PLATFORM_MAC
  return fmt::format("librexinput_{}{}.dylib", name, postfix);
#else
  return fmt::format("librexinput_{}{}.so", name, postfix);
#endif
}

// Plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown.
std::vector<platform::DynamicLibrary>& LoadedPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IInputSystem> LoadInputPlugin(std::string_view name, bool tool_mode,
                                              InputAssignmentPolicy assignment) {
  auto path = rex::filesystem::GetExecutableFolder() / PluginFileName(name);
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR(
        "Input plugin '{}' not found at {}. Stage it next to the executable "
        "(INPUT_PLUGINS {} in rexglue_configure_target).",
        name, path.string(), name);
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("Input plugin '{}' failed to load: {}", name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<InputAbiVersionFn>(kInputAbiVersionSymbol);
  auto create_fn = library.GetSymbol<InputCreateFn>(kInputCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("Input plugin '{}' is not a rexglue input plugin (missing {} / {} exports): {}",
                 name, kInputAbiVersionSymbol, kInputCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kInputPluginAbiVersion) {
    REXSYS_ERROR("Input plugin '{}' has ABI version {}, host expects {}: {}", name, plugin_abi,
                 kInputPluginAbiVersion, path.string());
    return nullptr;
  }

  InputCreateInfo info{};
  info.struct_size = sizeof(InputCreateInfo);
  info.tool_mode = tool_mode;
  info.assignment = assignment;

  IInputSystem* input_system = create_fn(kInputPluginAbiVersion, &info);
  if (!input_system) {
    REXSYS_ERROR("Input plugin '{}' factory returned no input system", name);
    return nullptr;
  }

  LoadedPlugins().push_back(std::move(library));
  REXSYS_DEBUG("Input plugin '{}' loaded ({})", name, path.filename().string());
  return std::unique_ptr<IInputSystem>(input_system);
}

}  // namespace rex::system
