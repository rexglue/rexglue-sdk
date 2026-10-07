/**
 * @file        graphics/backend_select.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <format>
#include <span>
#include <string_view>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::gpu {

/** A host graphics API the Xenos backend runs on. */
enum class HostAPI : u8 {
  kD3D12,
  kVulkan,
  kNull,
};

#if REX_PLATFORM_WIN32
/** The host APIs this platform builds, in the order "any" tries them. */
inline constexpr std::array kPlatformHostAPIs{HostAPI::kD3D12, HostAPI::kVulkan};
/** The platform name used in selection errors. */
inline constexpr std::string_view kPlatformName = "windows";
#elif REX_PLATFORM_MAC
/** The host APIs this platform builds, in the order "any" tries them. */
inline constexpr std::array kPlatformHostAPIs{HostAPI::kVulkan};
/** The platform name used in selection errors. */
inline constexpr std::string_view kPlatformName = "macos";
#else
/** The host APIs this platform builds, in the order "any" tries them. */
inline constexpr std::array kPlatformHostAPIs{HostAPI::kVulkan};
/** The platform name used in selection errors. */
inline constexpr std::string_view kPlatformName = "linux";
#endif

/**
 * Picks the host API a backend name selects.
 *
 * @param name "any", "d3d12", "vulkan" or "null". "null" is valid on every platform.
 * @param available The host APIs the platform builds, in preference order.
 * @return The host API, or an error naming the backend and the platform.
 */
[[nodiscard]] inline Result<HostAPI> SelectHostAPI(std::string_view name,
                                                   std::span<const HostAPI> available) {
  if (name == "null") {
    return HostAPI::kNull;
  }
  if (name == "any" && !available.empty()) {
    return available.front();
  }
  HostAPI wanted;
  if (name == "d3d12") {
    wanted = HostAPI::kD3D12;
  } else if (name == "vulkan") {
    wanted = HostAPI::kVulkan;
  } else {
    return Err<HostAPI>(ErrorCategory::Config, std::format("unknown GPU backend '{}'", name));
  }
  for (HostAPI api : available) {
    if (api == wanted) {
      return api;
    }
  }
  return Err<HostAPI>(ErrorCategory::Config,
                      std::format("GPU backend '{}' is not available on {}", name, kPlatformName));
}

}  // namespace rex::gpu
