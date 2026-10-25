/**
 * @file        ui/sdl3/input_capture.h
 * @brief       Whether the overlay wants the keyboard and the mouse
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <imgui.h>

namespace rex::ui {

/**
 * Copies the capture flags of ImGui, so other threads read them without touching ImGui. The UI
 * thread writes, any thread reads.
 */
class InputCapture {
 public:
  /**
   * Stores the flags of a frame. Call it on the UI thread after NewFrame.
   *
   * @param io The ImGui IO of the frame.
   */
  void Update(const ImGuiIO& io) {
    mouse_.store(io.WantCaptureMouse, std::memory_order_relaxed);
    keyboard_.store(io.WantCaptureKeyboard, std::memory_order_relaxed);
  }

  /** Clears both flags. */
  void Clear() {
    mouse_.store(false, std::memory_order_relaxed);
    keyboard_.store(false, std::memory_order_relaxed);
  }

  /**
   * Reports whether the overlay wants the mouse.
   *
   * @return True when it does.
   */
  [[nodiscard]] bool WantsMouse() const { return mouse_.load(std::memory_order_relaxed); }

  /**
   * Reports whether the overlay wants the keyboard.
   *
   * @return True when it does.
   */
  [[nodiscard]] bool WantsKeyboard() const { return keyboard_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> mouse_{false};
  std::atomic<bool> keyboard_{false};
};

}  // namespace rex::ui
