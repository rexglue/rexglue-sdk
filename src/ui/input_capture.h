/**
 * @file        ui/input_capture.h
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

/** Capture flags of the overlay. The UI thread writes them and any thread reads them. */
class InputCapture {
 public:
  /** Stores the flags of a frame. Call it on the UI thread after NewFrame. */
  void Update(const ImGuiIO& io) {
    mouse_.store(io.WantCaptureMouse, std::memory_order_relaxed);
    keyboard_.store(io.WantCaptureKeyboard, std::memory_order_relaxed);
  }

  void Clear() {
    mouse_.store(false, std::memory_order_relaxed);
    keyboard_.store(false, std::memory_order_relaxed);
  }

  [[nodiscard]] bool WantsMouse() const { return mouse_.load(std::memory_order_relaxed); }

  [[nodiscard]] bool WantsKeyboard() const { return keyboard_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> mouse_{false};
  std::atomic<bool> keyboard_{false};
};

}  // namespace rex::ui
