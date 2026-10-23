/**
 * @file        hid/key_capture.h
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
#include <span>
#include <vector>

#include <rex/types.h>
#include <rex/export.h>
#include <rex/hid.h>
#include <rex/hid/input_source.h>
#include <rex/hid/virtual_key.h>

namespace rex::hid {

/**
 * Returns the keys and mouse buttons a binding can name, in ascending VirtualKey order. The
 * modifier keys are not listed, a held modifier becomes part of the captured input instead.
 *
 * @return The keys.
 */
REX_API std::span<const VirtualKey> GetBindableKeys();

/** How a capture ends without an input. */
struct KeyCaptureConfig {
  /** How long Escape must be held before its release cancels instead of binding Escape. */
  std::chrono::milliseconds cancel_hold{800};
  /** The pad buttons that cancel when all are held, 1 bit per PadButton, 0 for none. */
  u32 pad_cancel_mask = (1u << static_cast<u32>(PadButton::kLeftTrigger)) |
                        (1u << static_cast<u32>(PadButton::kRightTrigger));
};

/**
 * Waits for the next key, mouse button, wheel step or pad button the user presses. A key is
 * reported once it is released, because a key still held when the binding changes would fire the
 * row that was bound. Nothing is reserved, a key swallowed to mean cancel could never be bound, so
 * the cancel is Escape held for KeyCaptureConfig::cancel_hold or the pad cancel buttons. UI thread
 * only.
 */
class REX_API KeyCapture {
 public:
  /**
   * @param input The keyboard and mouse state to watch, which must outlive the capture.
   * @param config How the capture is canceled.
   */
  explicit KeyCapture(const IHIDBackend& input, KeyCaptureConfig config = {});

  /**
   * Sets the pad buttons that are held now, polled by the application from its pad driver.
   *
   * @param buttons 1 bit per PadButton.
   */
  void SetPadButtons(u32 buttons);

  /** Starts a capture. Keys and buttons already held are ignored until they are released. */
  void Begin();

  /**
   * Checks for a new input. The key modifiers held at the press become the modifiers of a key
   * input, a wheel step and a pad button carry none. The capture ends when an input is returned or
   * when it is canceled.
   *
   * @param now The current time, which tests replace.
   * @return The input, or empty while none is complete or the capture is not running.
   */
  [[nodiscard]] std::optional<InputSource> Poll(
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  /**
   * Reports whether an input was pressed and is waiting for its release. The pad's own cancel
   * must stand down meanwhile, because a driver can turn the key being captured into a
   * cancel press.
   *
   * @return True from the press of the input until Poll reports it.
   */
  [[nodiscard]] bool IsPending() const;

  /**
   * Reports whether the last capture ended by cancel.
   *
   * @return True after a cancel, until the next Begin.
   */
  [[nodiscard]] bool WasCanceled() const;

 private:
  bool IsSlotDown(size_t slot) const;

  const IHIDBackend& input_;
  KeyCaptureConfig config_;
  std::vector<bool> previous_down_;
  u32 pad_buttons_ = 0;
  i32 wheel_seen_ = 0;
  i32 pending_ = -1;
  KeyModifiers pending_modifiers_ = KeyModifiers::kNone;
  std::chrono::steady_clock::time_point pending_since_{};
  bool capturing_ = false;
  bool canceled_ = false;
};

}  // namespace rex::hid
