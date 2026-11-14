/**
 * @file        hid/key_capture.h
 * @brief       The capture of the next input for a binding
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include <rex/hid/binding.h>
#include <rex/hid/input_system.h>
#include <rex/platform.h>

namespace rex::hid {

class KeyCapture final : public IKeyCapture {
 public:
  /**
   * @param input Keyboard and mouse state to watch, which must outlive the capture.
   * @param config Cancel behavior.
   */
  explicit KeyCapture(const IInputSystem& input, KeyCaptureConfig config = {});

  void SetPadButtons(u32 buttons) override;
  void Begin() override;
  [[nodiscard]] std::optional<InputSource> Poll(
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) override;
  [[nodiscard]] bool IsPending() const override;
  [[nodiscard]] bool WasCanceled() const override;

 private:
  bool IsSlotDown(size_t slot) const;

  const IInputSystem& input_;
  KeyCaptureConfig config_;
  std::vector<bool> previous_down_;
  u32 pad_buttons_ = 0;
  i32 wheel_seen_ = 0;
  i32 pending_ = -1;
  KeyModifiers pending_modifiers_ = KeyModifiers::kNone;
  std::chrono::steady_clock::time_point pending_since_;
  bool capturing_ = false;
  bool canceled_ = false;
};

}  // namespace rex::hid
