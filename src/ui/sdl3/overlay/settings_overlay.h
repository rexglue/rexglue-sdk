/**
 * @file        ui/sdl3/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save-to-config.
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
#include <rex/cvar.h>
#include <rex/macros.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class IUserInterface;
struct ModuleOverlayContext;

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
