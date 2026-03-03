/**
 ******************************************************************************
 * ReXGlue SDK — macOS Windowed App Context                                   *
 ******************************************************************************
 * Phase 2: NSApplication run-loop based WindowedAppContext for macOS.         *
 ******************************************************************************
 */

#pragma once

#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace ui {

class MacOSWindowedAppContext final : public WindowedAppContext {
 public:
  MacOSWindowedAppContext() = default;
  ~MacOSWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  /// Runs the NSApplication main event loop (blocks until quit).
  void RunLoop();
};

}  // namespace ui
}  // namespace rex
