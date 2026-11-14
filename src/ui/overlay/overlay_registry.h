/**
 * @file        ui/overlay/overlay_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/utility.h>
#include <rex/types.h>
#include <rex/ui/ui_system.h>

namespace rex::ui {

class BindTable;
class ImGuiHost;
struct ModuleOverlayContext;

class OverlayRegistry : public NonCopyable {
 public:
  OverlayRegistry(ImGuiHost& drawer, BindTable& binds, const ModuleOverlayContext& context);
  ~OverlayRegistry();

  bool IsAnyVisible() const;
  bool IsExclusiveVisible() const;
  [[nodiscard]] Result<void> Add(std::span<const OverlayConfig> table);
  void Show(std::string_view name, bool visible);
  void Toggle(std::string_view name);
  IDialog* Get(std::string_view name);
  void Clear();

  void SetChangeCallback(MoveOnlyFunction<void()> callback) { on_change_ = std::move(callback); }

 private:
  struct Entry {
    std::string name;
    std::unique_ptr<IDialog> overlay;
    bool bound = false;
  };

  Entry* Find(std::string_view name);

  ImGuiHost& drawer_;
  BindTable& binds_;
  const ModuleOverlayContext& context_;
  std::vector<Entry> entries_;
  MoveOnlyFunction<void()> on_change_;
};

}  // namespace rex::ui
