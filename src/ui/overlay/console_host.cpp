#include "ui/overlay/console_host.h"

#include <rex/ui/ui_system.h>

namespace rex::ui {

bool ConsoleDockHost::CanUndock() const {
  return false;
}

void ConsoleDockHost::Undock() {}

void ConsoleDockHost::Dock() {}

void ConsoleDockHost::Hide() {
  ui_.PostTask([this] { ui_.ShowOverlay("console", false); });
}

void ConsoleDockHost::ToggleWindow() {}

}  // namespace rex::ui
