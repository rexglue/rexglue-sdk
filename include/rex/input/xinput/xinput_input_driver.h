/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Sanjay Govind, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include <rex/input/input_driver.h>

namespace rex::input::xinput {

class XinputInputDriver final : public InputDriver {
 public:
  explicit XinputInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~XinputInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  void* module_;
  void* XInputGetCapabilities_;
  void* XInputGetState_;
  void* XInputGetStateEx_;
  void* XInputGetKeystroke_;
  void* XInputSetState_;
  void* XInputEnable_;
};

}  // namespace rex::input::xinput
