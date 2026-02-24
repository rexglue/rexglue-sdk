/**
 * @file        rex/ui/overlay/settings_overlay.h
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
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path);
  ~SettingsDialog();

  void ToggleVisible() { visible_ = !visible_; }
  bool IsVisible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
  std::filesystem::path config_path_;
  char search_buf_[128] = {};
  int selected_category_ = 0;
};

}  // namespace rex::ui
