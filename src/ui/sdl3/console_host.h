/**
 * @file        ui/sdl3/console_host.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/gpu/backend.h>
#include <rex/move_only_function.h>

#include "ui/sdl3/console/view.h"

namespace rex::ui {

class SDL3UserInterface;

class ConsoleDockHost final : public ConsoleHost {
 public:
  explicit ConsoleDockHost(SDL3UserInterface& ui) : ui_(ui) {}

  bool CanUndock() const override;
  void Undock() override;
  void Dock() override;
  void Hide() override;
  void ToggleWindow() override;

 private:
  void Defer(rex::MoveOnlyFunction<void()> action);
  void OpenWindow();

  SDL3UserInterface& ui_;
};

}  // namespace rex::ui
