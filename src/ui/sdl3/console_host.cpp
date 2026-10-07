#include "ui/sdl3/console_host.h"

#include <rex/logging.h>

#include "ui/sdl3/console/model.h"
#include "ui/sdl3/console/window.h"
#include "ui/sdl3/user_interface.h"

namespace rex::ui {

bool ConsoleDockHost::CanUndock() const {
  return ui_.gpu_ != nullptr && ui_.gpu_->has_presentation();
}

void ConsoleDockHost::Undock() {
  Defer([this] {
    ui_.console_->set_dock(ConsoleDock::kUndocked);
    ui_.overlays_->Show("console", false);
    OpenWindow();
  });
}

void ConsoleDockHost::Dock() {
  Defer([this] {
    ui_.console_window_.reset();
    ui_.console_->set_dock(ConsoleDock::kDocked);
    ui_.overlays_->Show("console", true);
  });
}

void ConsoleDockHost::Hide() {
  Defer([this] {
    if (ui_.console_->dock() == ConsoleDock::kUndocked) {
      ui_.console_window_.reset();
    } else {
      ui_.overlays_->Show("console", false);
    }
  });
}

void ConsoleDockHost::ToggleWindow() {
  Defer([this] {
    if (ui_.console_window_) {
      ui_.console_window_.reset();
    } else {
      OpenWindow();
    }
  });
}

void ConsoleDockHost::Defer(std::function<void()> action) {
  SDLWindowedAppContext* context = ui_.context_.get();
  if (!context) {
    return;
  }
  context->CallInUIThreadDeferred([this, action = std::move(action)] {
    if (ui_.console_ && ui_.overlays_) {
      action();
    }
  });
}

void ConsoleDockHost::OpenWindow() {
  auto window = ConsoleWindow::Create(*ui_.context_, *ui_.gpu_, *ui_.GetImGuiStyle(), ui_.style_,
                                      *ui_.console_, *this);
  if (!window) {
    REXLOG_ERROR(ui, "{}", window.error().message);
    ui_.console_->set_dock(ConsoleDock::kDocked);
    ui_.overlays_->Show("console", true);
    return;
  }
  ui_.console_window_ = std::move(*window);
}

}  // namespace rex::ui
