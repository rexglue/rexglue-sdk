/**
 * @file        rexglue/command/app_name.h
 * @brief       App name checks and case forms for the init command
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/string/text.h>
#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

namespace rexglue::cli {

struct AppNameParts {
  std::string original;

  std::string pascal_case;
  std::string upper_case;
};

inline bool ValidateProjectName(const std::string& input, std::string& error) {
  if (input.empty()) {
    error = "Project name must not be empty";
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(input[0]))) {
    error = "Project name must start with a letter";
    return false;
  }
  for (char c : input) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      error = "Project name contains invalid character '" + std::string(1, c) +
              "'. Only letters, digits and underscore are allowed, because the name starts the "
              "names of the generated C++ files and functions";
      return false;
    }
  }
  return true;
}

inline AppNameParts ParseAppName(const std::string& input) {
  constexpr auto kIsSeparator = [](char c) { return c == ' ' || c == '_' || c == '-'; };

  constexpr auto kToUpper = [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  };

  AppNameParts parts;
  parts.original = input;
  parts.pascal_case.reserve(input.size());
  parts.upper_case.reserve(input.size());

  const std::string lower = rex::string::utf8_lower_ascii(input);
  bool at_new_word = true;
  for (size_t index = 0; index < input.size(); ++index) {
    const char c = input[index];
    if (kIsSeparator(c)) {
      at_new_word = true;
      continue;
    }

    parts.upper_case += kToUpper(c);
    parts.pascal_case += at_new_word ? kToUpper(c) : lower[index];

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
