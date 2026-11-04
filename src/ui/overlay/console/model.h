/**
 * @file        ui/overlay/console/model.h
 * @brief       Console state and command execution without a window
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/types.h>
#include <rex/utility.h>
#include <rex/settings_registry.h>

namespace rex {
class ISettingsRegistry;
}  // namespace rex

namespace rex::ui {

struct ConsoleLine {
  log::Level level = log::Level::kInfo; /**< Level, for color and filtering. */
  std::string category;                 /**< Category name, console for command output. */
  std::string text;
  u64 sequence = 0; /**< Order among log messages and command output. */
  u64 id = 0;       /**< Unique among the model's lines, for selection. */
};

struct Completion {
  std::string name;        /**< Dotted cvar name, command name or built in. */
  std::string scope;       /**< Setting scope, empty for a built in. */
  std::string description; /**< Description column. */
  bool command = false;    /**< True when the candidate runs instead of setting a value. */
};

/** Where the console is drawn. */
enum class ConsoleDock : u8 {
  kDocked,    /**< In the title window, anchored top left. */
  kMinimized, /**< In the title window, title bar only. */
  kUndocked,  /**< In its own OS window. */
};

/** Console state that outlives every view. */
class ConsoleModel : public NonCopyable {
 public:
  /** Lines kept. */
  static constexpr size_t kMaxLines = 4096;
  /** History entries kept and saved. */
  static constexpr size_t kMaxHistory = 256;
  /** Candidates Complete returns at most. */
  static constexpr size_t kMaxCompletions = 64;

  /**
   * Builds the model and loads the history.
   *
   * @param settings The registry the console reads and sets.
   * @param capture The sink the console shows.
   * @param history_path File the history is loaded from and saved to. Empty keeps it in memory.
   */
  ConsoleModel(ISettingsRegistry& settings, log::LogCaptureSink& capture,
               std::filesystem::path history_path);

  /** Pulls new entries from the capture sink. */
  void Poll();

  /**
   * Runs an input line. clear empties the output and help lists every cvar. A cvar name alone
   * prints its value, a name and a value sets it, and a command name runs it with the rest of the
   * line. Adds the line to the history and saves it.
   */
  void Execute(std::string_view line);

  /**
   * Returns candidates for the input, sorted by scope then name, with built ins first. Empty once
   * the input has a space. It holds at most kMaxCompletions candidates.
   */
  std::vector<Completion> Complete(std::string_view input) const;

  /**
   * Returns the help line for the name the input starts with. For a cvar, it gives the type, value,
   * default, allowed values or range and description. For a command or built in, it gives the
   * description. It is empty for an unknown name.
   */
  std::string HelpLine(std::string_view input) const;

  /**
   * Returns an older history entry. The walk stops at the oldest entry. The result is empty with no
   * history.
   */
  std::string_view StepHistoryBack();

  /**
   * @return The entry, or empty past the newest.
   */
  std::string_view StepHistoryForward();

  /**
   * Returns the output lines that pass the filters, oldest first. The pointers point into
   * GetLines() and stay valid until the next Poll, Execute or Clear.
   */
  std::vector<const ConsoleLine*> GetVisibleLines() const;

  void Clear();

  const std::deque<ConsoleLine>& GetLines() const { return lines_; }
  const std::deque<std::string>& GetHistory() const { return history_; }
  std::string& GetInput() { return input_; }
  log::Level GetMinLevel() const { return min_level_; }
  void SetMinLevel(log::Level level) { min_level_ = level; }
  bool IsShown(std::string_view category) const;
  void SetShown(std::string_view category, bool shown);
  const std::set<std::string, std::less<>>& GetCategories() const { return categories_; }

  /** Returns the docked width. It is a fraction of the title window width. */
  f32 GetWidthFraction() const { return width_fraction_; }

  /** Returns the docked height. It is a fraction of the title window height. */
  f32 GetHeightFraction() const { return height_fraction_; }

  /**
   * Sets the docked size.
   *
   * @param width Fraction of the window width.
   * @param height Fraction of the window height.
   */
  void SetSize(f32 width, f32 height);

  ConsoleDock GetDock() const { return dock_; }
  void SetDock(ConsoleDock dock) { dock_ = dock; }

 private:
  void Add(log::Level level, std::string_view text);
  void Append(log::Level level, std::string_view category, std::string_view text, u64 sequence);
  void AddHistory(std::string_view line);
  void LoadHistory();
  void SaveHistory() const;
  void ListCvars();

  ISettingsRegistry& settings_;
  log::LogCaptureSink& capture_;
  std::filesystem::path history_path_;
  std::deque<ConsoleLine> lines_;
  std::deque<std::string> history_;
  std::vector<log::CapturedMessage> poll_buffer_;
  std::set<std::string, std::less<>> categories_;
  std::set<std::string, std::less<>> hidden_;
  std::string input_;
  u64 last_sequence_ = 0;
  u64 next_id_ = 0;
  size_t history_pos_ = 0;
  log::Level min_level_ = log::Level::kTrace;
  f32 width_fraction_ = 1.0f;
  f32 height_fraction_ = 0.45f;
  ConsoleDock dock_ = ConsoleDock::kDocked;
};

}  // namespace rex::ui
