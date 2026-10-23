/**
 * @file        hid/bindings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/cvar/registry.h>
#include <rex/hid/input_source.h>

namespace rex::hid {

/** Number of directions of a stick row, which are up, down, left and right. */
inline constexpr u8 kAxisDirections = 4;

/** Number of keys that can fill 1 direction of a stick row. */
inline constexpr u8 kAxisKeysPerDirection = 2;

/** Number of key halves in a stick row. */
inline constexpr u8 kMaxAxisKeyHalves = kAxisDirections * kAxisKeysPerDirection;

/**
 * Returns the stick row slot of a key half.
 *
 * @param direction The direction, 0 to kAxisDirections - 1.
 * @param alternate Which key of the direction, 0 for the first and 1 for the second.
 * @return The 1 based slot to store in InputSource::axis_slot.
 */
constexpr u8 GetAxisSlot(u8 direction, u8 alternate) {
  return static_cast<u8>(alternate * kAxisDirections + direction + 1);
}

/**
 * Returns the direction of a stick row slot.
 *
 * @param axis_slot A 1 based slot from InputSource::axis_slot.
 * @return The direction, 0 to kAxisDirections - 1.
 */
constexpr u8 GetAxisDirection(u8 axis_slot) {
  return static_cast<u8>((axis_slot - 1) % kAxisDirections);
}

/** One binding the application declares. */
struct BindingDesc {
  /** The name of the binding, the cvar is scope.name. */
  std::string_view name;
  /** The default text, a comma separated list of inputs. */
  std::string_view defaults;
  /** The cvar description. */
  std::string_view description;
  /** The group the binding belongs to, an application defined value that scopes conflicts. */
  u32 group = 0;
  /** The stick a row binds, kNone for a plain list of inputs. */
  PadAxes axes = PadAxes::kNone;
};

/**
 * The input bindings of an application, kept in 1 string cvar per binding. A plain binding is a
 * comma separated list of inputs and repeats are folded. In a stick row item N fills key half N,
 * an empty item keeps its position, at most kMaxAxisKeyHalves key halves are kept, and a whole
 * stick input such as LStick takes no position. UI thread only.
 */
class REX_API BindingSet {
 public:
  /**
   * Parses the defaults of every binding. The descriptions must outlive the set.
   *
   * @param descs The bindings, addressed by index everywhere else.
   * @param scope The cvar scope, 1 name without a dot.
   */
  explicit BindingSet(std::span<const BindingDesc> descs, std::string_view scope = "bind");
  /** Removes the cvars from the registry when they are still registered. */
  ~BindingSet();
  BindingSet(const BindingSet&) = delete;
  BindingSet& operator=(const BindingSet&) = delete;

  /**
   * Adds the cvars to a registry in which the scope is already reserved, and follows them from
   * then on. A value a config file or the command line carried for a cvar is read now.
   *
   * @param registry The registry, which must outlive the registration.
   * @return Ok, or the error of the registry.
   */
  [[nodiscard]] Result<void> Register(cvar::ICvarRegistry& registry);

  /** Removes the cvars from the registry. The sources keep their last values. */
  void Unregister();

  /**
   * Returns the number of bindings.
   *
   * @return The count.
   */
  [[nodiscard]] size_t GetCount() const;

  /**
   * Returns the inputs of a binding.
   *
   * @param binding The index into the descriptions.
   * @return The inputs in order, empty for an index out of range.
   */
  [[nodiscard]] std::span<const InputSource> GetSources(size_t binding) const;

  /**
   * Replaces 1 input of a binding, or appends it when the slot is past the end.
   *
   * @param binding The index into the descriptions.
   * @param slot The position in GetSources.
   * @param source The new input.
   * @return True when the cvar took the new text, false when the set is not registered, the index
   *         is out of range or the source names nothing.
   */
  bool SetSource(size_t binding, size_t slot, const InputSource& source);

  /**
   * Replaces all inputs of a binding.
   *
   * @param binding The index into the descriptions.
   * @param sources The new inputs.
   * @return True when the cvar took the new text, false when the set is not registered or the
   *         index is out of range.
   */
  bool SetSources(size_t binding, std::span<const InputSource> sources);

  /**
   * Removes every input of a binding.
   *
   * @param binding The index into the descriptions.
   * @return True when the cvar took the new text, false when the set is not registered or the
   *         index is out of range.
   */
  bool Clear(size_t binding);

  /**
   * Restores the default of a binding.
   *
   * @param binding The index into the descriptions.
   * @return True when the cvar was reset, false when the set is not registered or the index is
   *         out of range.
   */
  bool Reset(size_t binding);

  /**
   * Restores the default of every binding in a group.
   *
   * @param group The group value of the descriptions.
   * @return True when the group has a binding and every one was reset.
   */
  bool ResetGroup(u32 group);

  /**
   * Finds another binding of the same group that already uses an input.
   *
   * @param binding The index into the descriptions of the binding being edited.
   * @param source The input to look for, compared with IsSameInput.
   * @return The index of the first other binding that holds the input, or empty.
   */
  [[nodiscard]] std::optional<size_t> FindConflict(size_t binding, const InputSource& source) const;

  /**
   * Returns a counter that rises on every change of any binding.
   *
   * @return The generation, which starts above 0.
   */
  [[nodiscard]] u32 GetGeneration() const;

  /**
   * Returns the messages about text that did not parse since the last call and clears them.
   *
   * @return One message per ignored item, naming the binding.
   */
  [[nodiscard]] std::vector<std::string> TakeParseErrors();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::hid
