/**
 * @file        ui/sdl3/message_box.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string>

#include <rex/platform/process.h>
#include <rex/result.h>

struct SDL_Window;

namespace rex::ui::sdl3 {

[[nodiscard]] Result<u32> ShowSDLBox(MessageType type, const std::string& title,
                                     const std::string& body, std::span<const std::string> buttons,
                                     SDL_Window* parent);

[[nodiscard]] Result<u32> ShowNativeBox(MessageType type, const std::string& title,
                                        const std::string& body,
                                        std::span<const std::string> buttons, SDL_Window* parent);

}  // namespace rex::ui::sdl3
