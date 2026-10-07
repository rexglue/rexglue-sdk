#pragma once

#include <rex/gpu/backend.h>

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
  void Defer(std::move_only_function<void()> action);
  void OpenWindow();

  SDL3UserInterface& ui_;
};

}  // namespace rex::ui
