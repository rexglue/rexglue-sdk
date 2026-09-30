/**
 * @file        ui/console/window.h
 * @brief       The console in its own OS window
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/result.h>
#include <rex/types.h>

namespace rex::ui {

class ConsoleHost;
class ConsoleModel;
class GraphicsProvider;
class ImGuiDrawer;
class ImmediateDrawer;
class Presenter;
class Window;
class WindowedAppContext;

/** An OS window filled with a console view. Closing it docks the console back. */
class ConsoleWindow : public NonCopyable {
 public:
  /**
   * Opens the window.
   *
   * @param context The app context that owns the UI thread.
   * @param provider Creates the window's presenter and immediate drawer.
   * @param theme Drawer whose styles the window copies.
   * @param model The console model shown. Outlives the window.
   * @param host Receives dock changes. Outlives the window.
   * @return The window, or ErrorCategory::Runtime when the window or its presenter fails.
   */
  static Result<std::unique_ptr<ConsoleWindow>> Create(WindowedAppContext& context,
                                                       GraphicsProvider& provider,
                                                       ImGuiDrawer& theme, ConsoleModel& model,
                                                       ConsoleHost& host);

  /** Closes the window. */
  ~ConsoleWindow();

 private:
  class Dialog;
  class Listener;

  ConsoleWindow();

  std::unique_ptr<Window> window_;
  std::unique_ptr<Listener> listener_;
  std::unique_ptr<Presenter> presenter_;
  std::unique_ptr<ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ImGuiDrawer> drawer_;
  std::unique_ptr<Dialog> dialog_;
};

}  // namespace rex::ui
