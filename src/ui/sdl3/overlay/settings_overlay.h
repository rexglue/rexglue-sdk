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
#include <rex/types.h>
#include <rex/platform.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class IUserInterface;
struct ModuleOverlayContext;

class SettingsOverlayDialog : public ImGuiDialog {
 public:
  explicit SettingsOverlayDialog(const ModuleOverlayContext& context);
  ~SettingsOverlayDialog();

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
