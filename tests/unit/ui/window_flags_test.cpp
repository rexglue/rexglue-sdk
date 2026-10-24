/**
 * @file        tests/unit/ui/window_flags_test.cpp
 * @brief       The window flags follow the host API
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/gpu/backend_select.h>

#include "ui/sdl3/window_flags.h"

using rex::gpu::HostAPI;
using rex::ui::GetWindowFlags;

REX_TEST_CASE(
    "A Vulkan window has the Vulkan flag and every window is resizable, hidden and scaled",
    "[ui]") {
  CHECK((GetWindowFlags(HostAPI::kVulkan) & SDL_WINDOW_VULKAN) != 0);
  for (const HostAPI api : {HostAPI::kD3D12, HostAPI::kVulkan, HostAPI::kNull}) {
    const SDL_WindowFlags flags = GetWindowFlags(api);
    CHECK((flags & SDL_WINDOW_RESIZABLE) != 0);
    CHECK((flags & SDL_WINDOW_HIDDEN) != 0);
    CHECK((flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0);
  }
}

REX_TEST_CASE("A window for another host API is created without the Vulkan flag", "[ui]") {
  CHECK((GetWindowFlags(HostAPI::kD3D12) & SDL_WINDOW_VULKAN) == 0);
  CHECK((GetWindowFlags(HostAPI::kNull) & SDL_WINDOW_VULKAN) == 0);
}
