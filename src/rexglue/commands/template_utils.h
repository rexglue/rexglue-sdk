/**
 * @file        rexglue/commands/template_utils.h
 * @brief       Shared utilities for init and migrate commands
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace rexglue::cli {

struct AppNameParts {
  std::string snake_case;
  std::string pascal_case;
  std::string upper_case;
};

inline bool validate_app_name(const std::string& input, std::string& error) {
  if (input.empty()) {
    error = "App name must not be empty";
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(input[0]))) {
    error = "App name must start with a letter";
    return false;
  }
  for (char c : input) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != ' ') {
      error = "App name contains invalid character '" + std::string(1, c) +
              "'. Only alphanumeric, space, underscore, and dash are allowed";
      return false;
    }
  }
  return true;
}

inline AppNameParts parse_app_name(const std::string& input) {
  std::vector<std::string> words;
  std::string current;
  for (char c : input) {
    if (c == ' ' || c == '_' || c == '-') {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
    } else {
      current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (!current.empty()) {
    words.push_back(current);
  }

  AppNameParts parts;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i > 0) parts.snake_case += '_';
    parts.snake_case += words[i];
  }
  for (const auto& w : words) {
    std::string word = w;
    if (!word.empty() && std::isalpha(static_cast<unsigned char>(word[0]))) {
      word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
    }
    parts.pascal_case += word;
  }
  for (const auto& w : words) {
    for (char c : w) {
      parts.upper_case += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }
  return parts;
}

inline bool write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream file(path);
  if (!file) {
    REXLOG_ERROR("Failed to create file: {}", path.string());
    return false;
  }
  file << content;
  return true;
}

}  // namespace rexglue::cli
