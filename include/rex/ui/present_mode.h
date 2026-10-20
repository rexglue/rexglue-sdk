/**
 * @file        rex/ui/present_mode.h
 *
 * @brief       How the presenter shows frames, in words, for a game's log.
 *
 * The presenters set it when they make a swap chain, for example
 * "Vulkan immediate (no V-Sync, may tear)" or "Direct3D 12 flip, sync
 * interval 0, tearing allowed". Empty until then. Any thread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>
#include <string_view>

namespace rex::ui {

std::string GetPresentModeDescription();
void SetPresentModeDescription(std::string_view description);

}  // namespace rex::ui
