/**
 * @file        rexglue/commands/app_name.h
 * @brief       App name checks and case forms for the init command
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

namespace rexglue::cli {

struct AppNameParts {
  std::string original;

  std::string pascal_case;
  std::string upper_case;
};

inline bool ValidateAppName(const std::string& input, std::string& error) {
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

inline AppNameParts ParseAppName(const std::string& input) {
  constexpr auto kIsSeparator = [](char c) { return c == ' ' || c == '_' || c == '-'; };

  constexpr auto kToLower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  constexpr auto kToUpper = [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  };

  AppNameParts parts;
  parts.original.reserve(input.size());
  parts.pascal_case.reserve(input.size());
  parts.upper_case.reserve(input.size());

  bool at_new_word = true;
  for (char c : input) {
    if (kIsSeparator(c)) {
      at_new_word = true;
      continue;
    }

    parts.original += c;
    parts.upper_case += kToUpper(c);
    parts.pascal_case += at_new_word ? kToUpper(c) : kToLower(c);

    at_new_word = false;
  }

  return parts;
}

inline nlohmann::json NamesToJSON(const AppNameParts& names) {
  return {{"original", names.original},
          {"pascal_case", names.pascal_case},
          {"upper_case", names.upper_case}};
}

}  // namespace rexglue::cli
