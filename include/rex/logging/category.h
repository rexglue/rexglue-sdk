/**
 * @file        logging/category.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include <rex/logging/level.h>
#include <rex/macros.h>
#include <rex/types.h>

namespace rex::log {

class detail_access;

/**
 * Constant initialized log category. Declared with REXLOG_CATEGORY and registered at Connect with
 * ILogger::Register. Every binary that includes a declaration holds its own copy, and copies with
 * the same name share 1 level.
 */
class REX_API Category : public NonCopyable {
 public:
  /** Slot value of a copy that has not written yet. */
  static constexpr u16 kUnresolved = 0xFFFF;

  /**
   * Builds the descriptor.
   *
   * @param name Identifier, lowercase, unique across the process.
   * @param description Shown by the console and by ILogger::Categories.
   */
  constexpr Category(std::string_view name, std::string_view description)
      : name_(name), description_(description) {}

  /**
   * Returns the category name.
   *
   * @return The name.
   */
  constexpr std::string_view name() const { return name_; }

  /**
   * Returns the category description.
   *
   * @return The description.
   */
  constexpr std::string_view description() const { return description_; }

  /**
   * Reports whether a message at a level would be written.
   *
   * @param level The message level.
   * @return True when the level is at or above the category's effective level.
   */
  bool Enabled(Level level) const;

 private:
  friend class detail_access;

  std::string_view name_;
  std::string_view description_;
  mutable std::atomic<u16> slot_ = kUnresolved;
};

/** 1 row of ILogger::Categories. */
struct CategoryInfo {
  std::string name;           /**< Category name. */
  std::string description;    /**< Category description. */
  Level level = Level::kInfo; /**< Effective level. */
  std::optional<Level>
      explicit_level;      /**< Level set for this category, empty when it follows the default. */
  bool registered = false; /**< False for a category written to before any module registered it. */
};

namespace detail {

/**
 * Writes a message. The logging macros call it after Category::Enabled.
 *
 * @param category The category.
 * @param level The message level.
 * @param text The formatted message.
 */
REX_API void Write(const Category& category, Level level, std::string_view text);

/**
 * Writes a stub call. The first call of each stub is a warning, later calls are Dev.
 *
 * @param seen Per stub flag, set by the first call.
 * @param name Name of the stubbed function.
 * @param detail Text after the name, may be empty.
 */
REX_API void WriteStub(std::atomic_flag& seen, std::string_view name, std::string_view detail);

/**
 * Collects categories for ILogger::Register. REXLOG_TABLE calls it.
 *
 * @tparam Categories Category objects.
 * @param categories The categories.
 * @return Their addresses.
 */
template <typename... Categories>
constexpr std::array<const Category*, sizeof...(Categories)> MakeTable(
    const Categories&... categories) {
  return {static_cast<const Category*>(&categories)...};
}

}  // namespace detail

}  // namespace rex::log
