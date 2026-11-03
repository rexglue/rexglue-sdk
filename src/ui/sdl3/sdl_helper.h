/**
 * @file        ui/sdl3/sdl_helper.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <vector>

#include <rex/result.h>
#include <rex/ui/dialog.h>
#include <rex/ui/window.h>

struct SDL_Window;

namespace rex::ui::sdl3 {

std::vector<Monitor> GetSDLDisplays();

std::vector<HostLocale> GetSDLPreferredLocales();

[[nodiscard]] Result<std::vector<std::filesystem::path>> ShowSDLFileDialog(
    const FileDialogConfig& config, SDL_Window* parent);

}  // namespace rex::ui::sdl3
