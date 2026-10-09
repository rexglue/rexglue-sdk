/**
 * @file        ui/sdl3/bind_table.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/cvar/registry.h>
#include <rex/hid/key_chord.h>
#include <rex/macros.h>
#include <rex/types.h>
#include <rex/ui/ui_event.h>

namespace rex::ui {

class BindTable : public NonCopyable {
 public:
  BindTable() = default;
  ~BindTable();

  void Attach(cvar::ICvarRegistry* registry);
  void Register(std::string_view name, hid::KeyChord default_key, std::string_view description,
                std::move_only_function<void()> callback);
  void Unregister(std::string_view name);
  bool Process(const KeyEvent& e);

 private:
  struct Entry {
    std::string name;
    std::string cvar_name;
    std::string description;
    std::unique_ptr<cvar::Var<std::string>> var;
    cvar::CallbackHandle handle;
    hid::KeyChord chord;
    std::shared_ptr<std::move_only_function<void()>> callback;
    bool added = false;
  };

  void AddToRegistry(Entry& entry);
  void RemoveFromRegistry(Entry& entry);
  void OnChanged(std::string_view name, std::string_view value);

  std::mutex mutex_;
  std::deque<Entry> entries_;
  cvar::ICvarRegistry* registry_ = nullptr;
};

}  // namespace rex::ui
