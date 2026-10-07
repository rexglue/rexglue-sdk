/**
 * @file        ui/overlay.cpp
 * @brief       Overlay tables and the persistent overlay registry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/overlay.h>

#include <algorithm>
#include <format>

#include <rex/gpu/overlay.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>

namespace rex::ui {

OverlayContext::OverlayContext(ImGuiDrawer& drawer,
                               const std::unique_ptr<gpu::IOverlayDrawer>& overlay_drawer,
                               cvar::ICvarRegistry& cvars, log::CaptureSink& log_capture,
                               const Config& config, const std::unique_ptr<Runtime>& runtime,
                               const DebugOverlayDialog::FrameStatsProvider& frame_stats)
    : drawer_(drawer),
      overlay_drawer_(overlay_drawer),
      cvars_(cvars),
      log_capture_(log_capture),
      console_(console),
      console_host_(console_host),
      config_(config),
      runtime_(runtime),
      frame_stats_(frame_stats) {}

ImGuiDrawer& OverlayContext::drawer() const {
  return drawer_;
}
gpu::IOverlayDrawer* OverlayContext::overlay_drawer() const {
  return overlay_drawer_.get();
}
cvar::ICvarRegistry& OverlayContext::cvars() const {
  return cvars_;
}
log::CaptureSink& OverlayContext::log_capture() const {
  return log_capture_;
}
ConsoleModel& OverlayContext::console() const {
  return console_;
}
ConsoleHost& OverlayContext::console_host() const {
  return console_host_;
}
const Config& OverlayContext::config() const {
  return config_;
}
Runtime* OverlayContext::runtime() const {
  return runtime_.get();
}
const DebugOverlayDialog::FrameStatsProvider& OverlayContext::frame_stats() const {
  return frame_stats_;
}

OverlayRegistry::OverlayRegistry(Window& window, ImGuiDrawer& drawer, const OverlayContext& context)
    : window_(window), drawer_(drawer), context_(context) {
  window_.AddListener(this);
}

OverlayRegistry::~OverlayRegistry() {
  window_.RemoveListener(this);
  Clear();
}

void OverlayRegistry::Clear() {
  for (Entry& entry : entries_) {
    if (entry.bound) {
      UnregisterBind(entry.name);
    }
    if (entry.visible) {
      drawer_.RemoveDialog(entry.overlay.get());
    }
  }
  entries_.clear();
}

void OverlayRegistry::OnClosing(UIEvent&) {
  Clear();
}

OverlayRegistry::Entry* OverlayRegistry::Find(std::string_view name) {
  auto it = std::ranges::find(entries_, name, &Entry::name);
  return it == entries_.end() ? nullptr : &*it;
}

Result<void> OverlayRegistry::Add(std::span<const OverlayDesc> table) {
  for (const OverlayDesc& desc : table) {
    if (Find(desc.name)) {
      return Err<void>(ErrorCategory::Config, std::format("overlay '{}' exists", desc.name));
    }
    Entry entry;
    entry.name = std::string(desc.name);
    entry.overlay = desc.create(context_);
    entry.overlay->SetOwned(true);
    drawer_.RemoveDialog(entry.overlay.get());
    if (!desc.default_key.empty()) {
      RegisterBind(entry.name, desc.default_key, std::format("Toggle the {} overlay", entry.name),
                   [this, name = entry.name] { Toggle(name); });
      entry.bound = true;
    }
    entries_.push_back(std::move(entry));
  }
  return Ok();
}

void OverlayRegistry::Show(std::string_view name, bool visible) {
  Entry* entry = Find(name);
  if (!entry || entry->visible == visible) {
    return;
  }
  entry->visible = visible;
  if (visible) {
    drawer_.AddDialog(entry->overlay.get());
    entry->overlay->OnShow();
  } else {
    drawer_.RemoveDialog(entry->overlay.get());
  }
}

void OverlayRegistry::Toggle(std::string_view name) {
  if (Entry* entry = Find(name)) {
    if (!entry->visible && entry->overlay->OnToggle()) {
      return;
    }
    Show(name, !entry->visible);
  }
}

ImGuiDialog* OverlayRegistry::Get(std::string_view name) {
  Entry* entry = Find(name);
  return entry ? entry->overlay.get() : nullptr;
}

bool OverlayRegistry::AnyVisible() const {
  return std::ranges::any_of(entries_, &Entry::visible);
}

REX_OVERLAY_TABLE(kStandardOverlayTable, REX_OVERLAY(DebugOverlayDialog, "debug", "F3"),
                  REX_OVERLAY(ConsoleDialog, "console", "Backtick"),
                  REX_OVERLAY(SettingsDialog, "settings", "F4"),
                  REX_OVERLAY(AchievementsOverlayDialog, "achievements", "F7"));

std::span<const OverlayDesc> detail::StandardOverlays() {
  return kStandardOverlayTable;
}

}  // namespace rex::ui
