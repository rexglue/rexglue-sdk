/**
 * @file        ui/sdl3/overlay/settings_overlay.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <rex/cvar.h>
#include <rex/macros.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class IUserInterface;
struct ModuleOverlayContext;

/**
 * Clamps the text of an integer cvar to its type range and constraints.
 *
 * @param desc The cvar, of type Int32, Int64, UInt32 or UInt64.
 * @param text The value text. Text that does not parse reads as 0.
 * @return The clamped value text.
 */
std::string ClampIntegerCvarText(const rex::cvar::Desc& desc, std::string_view text);

class SettingsDialog : public ImGuiDialog {
 public:
  explicit SettingsDialog(const ModuleOverlayContext& context);
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  IUserInterface& ui_;
  rex::cvar::ICvarRegistry& registry_;
  std::filesystem::path config_path_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
};

}  // namespace rex::ui
