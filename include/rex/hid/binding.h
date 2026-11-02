/**
 * @file        hid/binding.h
 * @brief       Input bindings and key capture
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/hid/input_keys.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/settings.h>
#include <rex/types.h>

namespace rex::hid {

inline constexpr u8 kAxisDirections = 4;

inline constexpr u8 kAxisKeysPerDirection = 2;

inline constexpr u8 kMaxAxisKeyHalves = kAxisDirections * kAxisKeysPerDirection;

/**
 * Returns the stick row slot of a key half. The slot is 1 based, to store in
 * InputSource::axis_slot.
 *
 * @param direction The direction, 0 to kAxisDirections - 1.
 * @param alternate Which key of the direction, 0 for the first and 1 for the second.
 */
constexpr u8 GetAxisSlot(u8 direction, u8 alternate) {
  return static_cast<u8>(alternate * kAxisDirections + direction + 1);
}

/**
 * Returns the direction of a stick row slot, 0 to kAxisDirections - 1.
 *
 * @param axis_slot A 1 based slot from InputSource::axis_slot.
 */
constexpr u8 GetAxisDirection(u8 axis_slot) {
  return static_cast<u8>((axis_slot - 1) % kAxisDirections);
}

struct BindingConfig {
  /** The name of the binding. The setting is scope.name. */
  std::string_view name;
  /** The default text, a comma separated list of inputs. */
  std::string_view defaults;
  std::string_view description;
  /** The group the binding belongs to, an application defined value that scopes conflicts. */
  u32 group = 0;
  /** The stick a row binds, kNone for a plain list of inputs. */
  PadAxes axes = PadAxes::kNone;
};

/**
 * The input bindings of an application, one string setting per binding. A plain binding is a comma
 * separated list of inputs. In a stick row item N fills key half N, an empty item keeps its
 * position, and a whole stick input such as LStick takes no position. Text with a bad item is
 * rejected as a whole and TakeParseErrors names each bad item. Create one with
 * IInputSystem::CreateBindingSet. UI thread only.
 */
class IBindingSet {
 public:
  virtual ~IBindingSet() = default;

  /**
   * Registers the settings and follows them. Values from a config file or the command line are read
   * now.
   *
   * @param settings The registry, which must outlive the registration.
   * @return Ok, or the error of the registry.
   */
  [[nodiscard]] virtual Result<void> Register(ISettings& settings) = 0;

  /** Removes the settings from the registry. The sources keep their last values. */
  virtual void Unregister() = 0;

  [[nodiscard]] virtual size_t GetCount() const = 0;

  /**
   * @param binding The index into the descriptions.
   * @return The inputs in order, empty for an index out of range.
   */
  [[nodiscard]] virtual std::span<const InputSource> GetSources(size_t binding) const = 0;

  /**
   * Replaces one input of a binding, or appends it when the slot is past the end.
   *
   * @param binding The index into the descriptions.
   * @param slot The position in GetSources.
   * @return True when the setting took the new text, false when the set is not registered, the
   * index is out of range or the source names nothing.
   */
  virtual bool SetSource(size_t binding, size_t slot, const InputSource& source) = 0;

  /**
   * Replaces all inputs of a binding.
   *
   * @param binding The index into the descriptions.
   * @return True when the setting took the new text, false when the set is not registered or the
   *         index is out of range.
   */
  virtual bool SetSources(size_t binding, std::span<const InputSource> sources) = 0;

  /**
   * Removes every input of a binding.
   *
   * @param binding The index into the descriptions.
   * @return True when the setting took the new text, false when the set is not registered or the
   *         index is out of range.
   */
  virtual bool Clear(size_t binding) = 0;

  /**
   * Restores the default of a binding.
   *
   * @param binding The index into the descriptions.
   * @return True when the setting was reset, false when the set is not registered or the index is
   *         out of range.
   */
  virtual bool Reset(size_t binding) = 0;

  /**
   * Restores the default of every binding in a group.
   *
   * @param group The group value of the descriptions.
   * @return True when the group has a binding and every one was reset.
   */
  virtual bool ResetGroup(u32 group) = 0;

  /**
   * Finds another binding of the same group that already uses an input.
   *
   * @param binding The index into the descriptions of the binding being edited.
   * @param source The input to look for, compared with IsSameInput.
   * @return The index of the first other binding that holds the input, or empty.
   */
  [[nodiscard]] virtual std::optional<size_t> FindConflict(size_t binding,
                                                           const InputSource& source) const = 0;

  /**
   * Finds the binding behind a setting.
   *
   * @param setting_name The full setting name, scope.name.
   * @return The index into the descriptions, or empty when no binding has the setting.
   */
  [[nodiscard]] virtual std::optional<size_t> FindBySetting(
      std::string_view setting_name) const = 0;

  /** Returns a counter that rises on every change of any binding, which starts above 0. */
  [[nodiscard]] virtual u32 GetGeneration() const = 0;

  /**
   * Returns the messages about text that did not parse since the last call and clears them. There
   * is one message per bad item, naming the binding.
   */
  [[nodiscard]] virtual std::vector<std::string> TakeParseErrors() = 0;
};

/**
 * Returns the keys and mouse buttons a binding can name, in ascending VirtualKey order. Modifier
 * keys are not listed.
 */
REX_HID_API std::span<const VirtualKey> GetBindableKeys();

/** How a capture ends without an input. */
struct KeyCaptureConfig {
  /** How long Escape must be held before its release cancels instead of binding Escape. */
  std::chrono::milliseconds cancel_hold{800};
  /** The pad buttons that cancel when all are held, 1 bit per PadInput, 0 for none. */
  u32 pad_cancel_mask = (1u << static_cast<u32>(PadInput::kLeftTrigger)) |
                        (1u << static_cast<u32>(PadInput::kRightTrigger));
};

/**
 * Waits for the next key, mouse button, wheel step or pad button the user presses. A key is
 * reported once released. Cancel is Escape held for KeyCaptureConfig::cancel_hold or the pad cancel
 * buttons. Create one with IInputSystem::CreateKeyCapture. UI thread only.
 */
class IKeyCapture {
 public:
  virtual ~IKeyCapture() = default;

  /**
   * Sets the pad buttons that are held now, polled by the application from its pad driver.
   *
   * @param buttons 1 bit per PadInput.
   */
  virtual void SetPadButtons(u32 buttons) = 0;

  /** Starts a capture. Keys and buttons held at the start are ignored until released. */
  virtual void Begin() = 0;

  /**
   * Returns the next completed input, or empty while none is complete or the capture is not
   * running. Modifiers held at the press join a key input. The capture ends when an input is
   * returned or canceled.
   *
   * @param now The current time, which tests replace.
   */
  [[nodiscard]] virtual std::optional<InputSource> Poll(
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) = 0;

  /**
   * Reports whether an input was pressed and is waiting for its release. The pad cancel buttons are
   * ignored until Poll reports the input.
   *
   * @return True from the press of the input until Poll reports it.
   */
  [[nodiscard]] virtual bool IsPending() const = 0;

  /**
   * Reports whether the last capture ended by cancel.
   *
   * @return True after a cancel, until the next Begin.
   */
  [[nodiscard]] virtual bool WasCanceled() const = 0;
};

}  // namespace rex::hid
