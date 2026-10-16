/**
 * @file        tests/unit/gpu/backend_select_test.cpp
 * @brief       Backend name selection of the Xenos module
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string>

#include <rex/gpu/backend_select.h>

using rex::gpu::HostAPI;
using rex::gpu::SelectHostAPI;

namespace {
constexpr std::array kWindows{HostAPI::kD3D12, HostAPI::kVulkan};
constexpr std::array kVulkanOnly{HostAPI::kVulkan};
}  // namespace

REX_TEST_CASE("any picks the first host API the platform builds", "[gpu][backend]") {
  CHECK(SelectHostAPI("any", kWindows).value() == HostAPI::kD3D12);
  CHECK(SelectHostAPI("any", kVulkanOnly).value() == HostAPI::kVulkan);
}

REX_TEST_CASE("A named host API is picked when the platform builds it", "[gpu][backend]") {
  CHECK(SelectHostAPI("d3d12", kWindows).value() == HostAPI::kD3D12);
  CHECK(SelectHostAPI("vulkan", kWindows).value() == HostAPI::kVulkan);
  CHECK(SelectHostAPI("vulkan", kVulkanOnly).value() == HostAPI::kVulkan);
}

REX_TEST_CASE("A host API the platform lacks fails with its name and the platform",
              "[gpu][backend]") {
  auto selected = SelectHostAPI("d3d12", kVulkanOnly);
  REQUIRE_FALSE(selected);
  CHECK(selected.error().message.find("d3d12") != std::string::npos);
  CHECK(selected.error().message.find(rex::gpu::kPlatformName) != std::string::npos);
}

REX_TEST_CASE("An unknown backend name fails with the name", "[gpu][backend]") {
  auto selected = SelectHostAPI("metal", kWindows);
  REQUIRE_FALSE(selected);
  CHECK(selected.error().message.find("metal") != std::string::npos);
}

REX_TEST_CASE("The platform list matches the platform rule", "[gpu][backend]") {
#if REX_PLATFORM_WIN32
  CHECK(rex::gpu::kPlatformHostAPIs.size() == 2);
  CHECK(rex::gpu::kPlatformHostAPIs[0] == HostAPI::kD3D12);
#else
  CHECK(rex::gpu::kPlatformHostAPIs.size() == 1);
  CHECK(rex::gpu::kPlatformHostAPIs[0] == HostAPI::kVulkan);
#endif
}

REX_TEST_CASE("null is picked on every platform and never by any", "[gpu][backend]") {
  CHECK(SelectHostAPI("null", kWindows).value() == HostAPI::kNull);
  CHECK(SelectHostAPI("null", kVulkanOnly).value() == HostAPI::kNull);
  CHECK(SelectHostAPI("null", rex::gpu::kPlatformHostAPIs).value() == HostAPI::kNull);
  CHECK(SelectHostAPI("any", rex::gpu::kPlatformHostAPIs).value() != HostAPI::kNull);
}

REX_TEST_CASE("A misspelled null fails with the name", "[gpu][backend]") {
  auto selected = SelectHostAPI("nul", rex::gpu::kPlatformHostAPIs);
  REQUIRE_FALSE(selected);
  CHECK(selected.error().message.find("nul") != std::string::npos);
}
