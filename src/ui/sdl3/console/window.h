/**
 * @file        ui/sdl3/console/window.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/platform.h>
#include <rex/gpu.h>
#include <rex/result.h>
#include <rex/types.h>

struct ImGuiStyle;

namespace rex::ui {

class ConsoleHost;
class ConsoleModel;
class ImGuiDrawer;
struct Style;
class Window;
class WindowedAppContext;

/** An OS window filled with a console view. Closing it docks the console back. */
class ConsoleWindow : public NonCopyable {
  struct Key {};

 public:
  /**
   * Opens the window.
   *
   * @param context The app context that owns the UI thread.
   * @param backend Creates the window's presenter and overlay drawer.
   * @param imgui_style The ImGui style the window copies.
   * @param style The overlay style the window copies.
   * @param model The console model shown. Outlives the window.
   * @param host Receives dock changes. Outlives the window.
   * @return The window, or ErrorCategory::kRuntime when the window or its presenter fails.
   */
  [[nodiscard]] static Result<std::unique_ptr<ConsoleWindow>> Create(
      WindowedAppContext& context, gpu::IGPUBackend& backend, const ImGuiStyle& imgui_style,
      const Style& style, ConsoleModel& model, ConsoleHost& host);

  /**
   * Builds an empty window. Only Create can make the key.
   *
   * @param key The private construction key.
   */
  explicit ConsoleWindow(Key);

  /** Closes the window. */
  ~ConsoleWindow();

 private:
  class Dialog;
  class Listener;

  std::unique_ptr<Window> window_;
  std::unique_ptr<Listener> listener_;
  std::unique_ptr<gpu::IPresenter> presenter_;
  std::unique_ptr<gpu::IOverlayDrawer> overlay_drawer_;
  std::unique_ptr<ImGuiDrawer> drawer_;
  std::unique_ptr<Dialog> dialog_;
};

}  // namespace rex::ui
