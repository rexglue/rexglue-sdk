/**
 * @file        system/gpu_plugin_loader.cpp
 * @brief       Host-side loader for GPU emulation plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/gpu_plugin.h>

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>
#include <rex/string.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
  return fmt::format("rexgpu-{}{}.dll", name, postfix);
#elif REX_PLATFORM_MAC
  return fmt::format("librexgpu-{}{}.dylib", name, postfix);
#else
  return fmt::format("librexgpu-{}{}.so", name, postfix);
#endif
}

// The file names the plugin can have next to the executable. A MinGW build
// of the SDK names its DLLs with CMake's "lib" prefix (librexgpu-xenos.dll),
// an MSVC build without it; both are accepted.
std::vector<std::filesystem::path> PluginCandidates(std::string_view name) {
  auto folder = rex::filesystem::GetExecutableFolder();
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(folder / PluginFileName(name));
#if REX_PLATFORM_WIN32
  candidates.push_back(folder / ("lib" + PluginFileName(name)));
#endif
  return candidates;
}

// Why the last library load failed, in Windows' words (" (error N: text)").
// On Linux and macOS the loader itself prints dlopen's message.
std::string LoadErrorText() {
#if REX_PLATFORM_WIN32
  DWORD code = GetLastError();
  wchar_t* text = nullptr;
  DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::string message = fmt::format("error {}", code);
  if (length && text) {
    std::wstring_view wide(text, length);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
      wide.remove_suffix(1);
    }
    message += ": " + rex::string::to_utf8(std::u16string_view(
                          reinterpret_cast<const char16_t*>(wide.data()), wide.size()));
  }
  if (text) {
    LocalFree(text);
  }
  if (code == ERROR_MOD_NOT_FOUND || code == ERROR_PROC_NOT_FOUND) {
    // Windows reports the plugin's own name even when what is missing is a
    // DLL the plugin imports.
    message += "; the plugin file exists, so a DLL it needs is missing or too old next to the executable";
  }
  return " (" + message + ")";
#else
  return "";
#endif
}

// Plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown.
std::vector<platform::DynamicLibrary>& LoadedPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
  auto candidates = PluginCandidates(name);
  std::filesystem::path path;
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      path = candidate;
      break;
    }
  }
  if (path.empty()) {
    std::string searched;
    for (const auto& candidate : candidates) {
      searched += (searched.empty() ? "" : ", ") + candidate.string();
    }
    REXSYS_ERROR(
        "GPU plugin '{}' not found; looked for {}. Stage it next to the executable "
        "(GPU_PLUGINS {} in rexglue_configure_target).",
        name, searched, name);
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("GPU plugin '{}' failed to load: {}{}", name, path.string(), LoadErrorText());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<GpuAbiVersionFn>(kGpuAbiVersionSymbol);
  auto create_fn = library.GetSymbol<GpuCreateFn>(kGpuCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("GPU plugin '{}' is not a rexglue GPU plugin (missing {} / {} exports): {}", name,
                 kGpuAbiVersionSymbol, kGpuCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kGpuPluginAbiVersion) {
    REXSYS_ERROR("GPU plugin '{}' has ABI version {}, host expects {}: {}", name, plugin_abi,
                 kGpuPluginAbiVersion, path.string());
    return nullptr;
  }

  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();

  IGraphicsSystem* graphics_system = create_fn(kGpuPluginAbiVersion, &info);
  if (!graphics_system) {
    REXSYS_ERROR("GPU plugin '{}' factory returned no graphics system (backend '{}')", name,
                 backend_str);
    return nullptr;
  }

  LoadedPlugins().push_back(std::move(library));
  REXSYS_DEBUG("GPU plugin '{}' loaded ({})", name, path.filename().string());
  return std::unique_ptr<IGraphicsSystem>(graphics_system);
}

}  // namespace rex::system
