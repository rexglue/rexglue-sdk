/**
 * @file        ui/window_settings.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/window.h>
#include <rex/ui/ui_system.h>

#include "ui/ui_state.h"
#include "ui/video_mode.h"

namespace rex::ui {

void UISystem::State::WatchWindowSettings() {
  auto on_ui_thread = [this](auto fn) {
    return [this, fn](const auto&) {
      owner.PostTask([this, fn] {
        if (window_open.load()) {
          fn();
        }
      });
    };
  };
  auto apply_fullscreen = [this] {
    u32 mode_width = 0;
    u32 mode_height = 0;
    ui::detail::ResolveModeSize(owner.GetSettings(), mode_width, mode_height);
    driver->SetFullscreen(owner.GetSettings().fullscreen.Get(),
                          owner.GetSettings().fullscreen_exclusive.Get(),
                          owner.GetSettings().monitor.Get(), mode_width, mode_height);
  };
  auto apply_size = [this] {
    i32 width = 0;
    i32 height = 0;
    if (ui::detail::ResolveConfiguredSize(owner.GetSettings(), width, height) && width > 0 &&
        height > 0) {
      driver->SetSize(static_cast<u32>(width), static_cast<u32>(height));
    }
  };
  auto apply_limits = [this] {
    driver->SetLimits(static_cast<u32>(owner.GetSettings().min_width.Get()),
                      static_cast<u32>(owner.GetSettings().min_height.Get()),
                      ui::detail::ParseAspectRatio(owner.GetSettings().aspect_ratio.Get()));
  };
  watches.push_back(owner.GetSettings().fullscreen.OnChange(on_ui_thread(apply_fullscreen)));
  watches.push_back(
      owner.GetSettings().fullscreen_exclusive.OnChange(on_ui_thread(apply_fullscreen)));
  watches.push_back(owner.GetSettings().monitor.OnChange(on_ui_thread(apply_fullscreen)));
  watches.push_back(owner.GetSettings().window_width.OnChange(on_ui_thread(apply_size)));
  watches.push_back(owner.GetSettings().window_height.OnChange(on_ui_thread(apply_size)));
  watches.push_back(
      owner.GetSettings().resolution.OnChange(on_ui_thread([apply_size, apply_fullscreen] {
        apply_size();
        apply_fullscreen();
      })));
  watches.push_back(owner.GetSettings().min_width.OnChange(on_ui_thread(apply_limits)));
  watches.push_back(owner.GetSettings().min_height.OnChange(on_ui_thread(apply_limits)));
  watches.push_back(owner.GetSettings().aspect_ratio.OnChange(on_ui_thread(apply_limits)));
  watches.push_back(owner.GetSettings().title.OnChange(on_ui_thread([this] {
    const std::string text = owner.GetSettings().title.Get();
    driver->SetTitle(text.empty() ? std::string_view("ReXGlue") : std::string_view(text));
  })));
  watches.push_back(owner.GetSettings().cursor_timeout.OnChange(on_ui_thread([this] {
    last_pointer_motion = std::chrono::steady_clock::now();
    if (cursor_hidden_by_timeout) {
      cursor_hidden_by_timeout = false;
      if (applied_pointer_mode == hid::PointerMode::kFree) {
        driver->SetCursorShape(true, rex::ui::CursorShape::kArrow);
      }
    }
  })));
}

}  // namespace rex::ui
