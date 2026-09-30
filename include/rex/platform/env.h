/**
 * @file        rex/platform/env.h
 * @brief       Platform-agnostic environment variable access (UTF-8).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::platform {

// Reads an environment variable. Returns std::nullopt when not set, or an
// optional containing UTF-8 bytes when set (an empty string is a distinct
// state from nullopt).
std::optional<std::string> GetEnvironmentValue(std::string_view name);

// Sets an environment variable, overwriting any existing value. Value is
// UTF-8. Returns true on success.
bool SetEnvironmentValue(std::string_view name, std::string_view value);

// Removes an environment variable. Returns true if the variable was unset on
// return (success, or already not set).
bool UnsetEnvironmentValue(std::string_view name);

/**
 * Every environment variable of the process as name and value pairs.
 */
std::vector<std::pair<std::string, std::string>> EnumerateEnvironment();

}  // namespace rex::platform
