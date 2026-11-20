/**
 * @file        ui/window_settings.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>
#include <utility>

#include <rex/setting.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_system.h>

#include "ui/ui_state.h"
#include "ui/video_mode.h"

namespace rex::ui {

UISystem::State::AppliedFullscreen UISystem::State::ResolveFullscreen() const {
  const UISettings& settings = owner.GetSettings();
  AppliedFullscreen out{settings.fullscreen.Get(), settings.fullscreen_exclusive.Get(),
                        settings.monitor.Get(), 0, 0};
  ui::detail::ResolveModeSize(settings, out.mode_width, out.mode_height);
  return out;
}

UISystem::State::AppliedLimits UISystem::State::ResolveLimits() const {
  const UISettings& settings = owner.GetSettings();
  return {static_cast<u32>(settings.min_width.Get()), static_cast<u32>(settings.min_height.Get()),
          ui::detail::ParseAspectRatio(settings.aspect_ratio.Get())};
}

void UISystem::State::WatchWindowSettings() {
  const auto on_ui_thread = [this](auto fn) {
    owner.PostTask([this, fn] {
      if (window_open.load()) {
        fn();
      }
    });
  };
  const auto apply_fullscreen = [this] {
    const AppliedFullscreen wanted = ResolveFullscreen();
    if (applied_fullscreen == wanted) {
      return;
    }
    applied_fullscreen = wanted;
    driver->SetFullscreen(wanted.fullscreen, wanted.exclusive, wanted.monitor, wanted.mode_width,
                          wanted.mode_height);
  };
  const auto apply_size = [this] {
    i32 width = 0;
    i32 height = 0;
    if (!ui::detail::ResolveConfiguredSize(owner.GetSettings(), width, height) || width <= 0 ||
        height <= 0) {
      return;
    }
    const std::pair wanted{static_cast<u32>(width), static_cast<u32>(height)};
    if (applied_size == wanted) {
      return;
    }
    applied_size = wanted;
    driver->SetSize(wanted.first, wanted.second);
  };
  const auto apply_limits = [this] {
    const AppliedLimits wanted = ResolveLimits();
    if (applied_limits == wanted) {
      return;
    }
    applied_limits = wanted;
    driver->SetLimits(wanted.min_width, wanted.min_height, wanted.ratio);
  };
  UISettings& settings = owner.GetSettings();
  watches.push_back(rex::Bind(
      [on_ui_thread, apply_fullscreen](const bool&, const bool&, const i32&, const std::string&) {
        on_ui_thread(apply_fullscreen);
      },
      settings.fullscreen, settings.fullscreen_exclusive, settings.monitor, settings.resolution));
  watches.push_back(
      rex::Bind([on_ui_thread, apply_size](const i32&, const i32&,
                                           const std::string&) { on_ui_thread(apply_size); },
                settings.window_width, settings.window_height, settings.resolution));
  watches.push_back(
      rex::Bind([on_ui_thread, apply_limits](const i32&, const i32&,
                                             const std::string&) { on_ui_thread(apply_limits); },
                settings.min_width, settings.min_height, settings.aspect_ratio));
  watches.push_back(settings.title.Bind([this, on_ui_thread](const std::string&) {
    on_ui_thread([this] {
      const std::string text = owner.GetSettings().title.Get();
      if (applied_title == text) {
        return;
      }
      applied_title = text;
      driver->SetTitle(text.empty() ? std::string_view("ReXGlue") : std::string_view(text));
    });
  }));
  watches.push_back(settings.cursor_timeout.OnChange([on_ui_thread, this](const f64&) {
    on_ui_thread([this] {
      last_pointer_motion = std::chrono::steady_clock::now();
      if (cursor_hidden_by_timeout) {
        cursor_hidden_by_timeout = false;
        if (applied_pointer_mode == PointerMode::kFree) {
          driver->SetCursorShape(true, rex::ui::CursorShape::kArrow);
        }
      }
    });
  }));
}

}  // namespace rex::ui
