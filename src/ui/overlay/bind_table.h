/**
 * @file        ui/overlay/bind_table.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/hid/input_keys.h>
#include <rex/hid/input_system.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::ui {

class BindTable : public NonCopyable {
 public:
  BindTable() = default;
  ~BindTable();

  void Attach(ISettings* settings);

  /**
   * Sets the input system that reads and writes the chords. Call it before Register. Without one,
   * binds keep their default chord and ignore changes to the setting text.
   */
  void SetInput(const hid::IInputSystem* input);
  void Register(std::string_view name, hid::KeyChord default_key, std::string_view description,
                rex::MoveOnlyFunction<void()> callback);
  void Unregister(std::string_view name);
  bool Process(const hid::InputEvent& e);

 private:
  class BindOwner final : public SettingOwner {
   public:
    BindOwner(std::string_view name, std::string_view default_text, std::string_view help)
        : SettingOwner("ui.bind"), chord(this, name, std::string(default_text), help, kKeyChord) {}

    Setting<std::string> chord;
  };

  struct Entry {
    std::string name;
    std::string setting_name;
    std::string description;
    std::unique_ptr<BindOwner> owner;
    ScopeGuard watch;
    hid::KeyChord chord;
    std::shared_ptr<rex::MoveOnlyFunction<void()>> callback;
    bool added = false;
  };

  void AddToRegistry(Entry& entry);
  void RemoveFromRegistry(Entry& entry);
  void OnChanged(std::string_view name, std::string_view value);

  std::mutex mutex_;
  std::deque<Entry> entries_;
  ISettings* settings_ = nullptr;
  const hid::IInputSystem* input_ = nullptr;
};

}  // namespace rex::ui
