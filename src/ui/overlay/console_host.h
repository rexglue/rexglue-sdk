/**
 * @file        ui/overlay/console_host.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "ui/overlay/console/view.h"

namespace rex {
namespace ui {
class IUISystem;
}  // namespace ui
}  // namespace rex

namespace rex::ui {

class ConsoleDockHost final : public IConsoleHost {
 public:
  explicit ConsoleDockHost(IUISystem& ui) : ui_(ui) {}

  bool CanUndock() const override;
  void Undock() override;
  void Dock() override;
  void Hide() override;
  void ToggleWindow() override;

 private:
  IUISystem& ui_;
};

}  // namespace rex::ui
