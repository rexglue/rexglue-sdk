/**
 * @file        platform/env.h
 * @brief       Environment variable access in UTF-8
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

#include <rex/macros.h>

namespace rex::platform {

/**
 * Reads an environment variable.
 *
 * @param name Variable name, UTF-8.
 * @return The value in UTF-8, or std::nullopt when the variable is not set. An empty value
 *         is distinct from std::nullopt.
 */
REX_API std::optional<std::string> GetEnvironmentValue(std::string_view name);

/**
 * Sets an environment variable, replacing any existing value.
 *
 * @param name Variable name, UTF-8.
 * @param value New value, UTF-8.
 * @return True on success.
 */
REX_API bool SetEnvironmentValue(std::string_view name, std::string_view value);

/**
 * Removes an environment variable.
 *
 * @param name Variable name, UTF-8.
 * @return True when the OS call succeeds.
 */
REX_API bool UnsetEnvironmentValue(std::string_view name);

/**
 * Lists every environment variable of the process.
 *
 * @return Name and value pairs in UTF-8.
 */
REX_API std::vector<std::pair<std::string, std::string>> EnumerateEnvironment();

}  // namespace rex::platform
