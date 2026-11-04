/**
 * @file        ui/overlay/settings_overlay.h
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
#include <rex/ui/ui_system.h>
#include <rex/platform.h>
#include <rex/settings_registry.h>

namespace rex {
namespace hid {
class InputSettings;
}  // namespace hid
}  // namespace rex

namespace rex::hid {
class IBindingSet;
class IInputSystem;
}  // namespace rex::hid

namespace rex::ui {

struct ModuleOverlayContext;

class SettingsOverlayDialog : public DialogBase {
 public:
  explicit SettingsOverlayDialog(const ModuleOverlayContext& context);
  ~SettingsOverlayDialog();

  bool Draw(ImGuiIO& io) override;

 private:
  IUISystem& ui_;
  rex::ISettingsRegistry& settings_;
  const rex::hid::IBindingSet* key_bindings_;
  const rex::hid::IInputSystem* input_;
  const rex::hid::InputSettings* input_settings_;
  std::filesystem::path config_path_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
};

}  // namespace rex::ui
