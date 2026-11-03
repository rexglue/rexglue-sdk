/**
 * @file        ui/dialog.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rex/platform/process.h>
#include <rex/types.h>

namespace rex::ui {

struct ChoiceConfig {
  MessageType type = MessageType::kError;
  std::string title;
  std::string body;
  /** Button labels. The first is the default and the last is the escape button. */
  std::vector<std::string> buttons;
};

/** A prompt shown inside the title window. */
struct PromptConfig {
  std::string title;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::string body = {};
  /** Button labels. Empty means a single OK button. */
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::vector<std::string> buttons = {};
  bool text_input = false;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::string default_text = {};
  /** Longest text the field accepts in characters. 0 means no limit. */
  u32 max_length = 0;
};

struct PromptResult {
  /** True when no button completed the prompt. */
  bool canceled = true;
  /** Index of the pressed button. */
  u32 button = 0;
  /** Text of the field when the prompt had one. */
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::string text = {};
};

struct FileFilter {
  std::string name;
  /** Extensions separated by semicolons, for example "iso;xex". */
  std::string pattern;
};

struct FileDialogConfig {
  enum class Mode : u8 {
    kOpenFile,
    kOpenFiles,
    kOpenFolder,
    kSaveFile, /**< One file that may not exist. */
  };
  Mode mode = Mode::kOpenFile;
  std::string title;
  /** File type filters. Empty accepts every file. */
  std::vector<FileFilter> filters;
  std::filesystem::path default_location;
};

}  // namespace rex::ui
