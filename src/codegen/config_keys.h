/**
 * @file        codegen/config_keys.h
 * @brief       Warnings for config keys the loader does not read
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

namespace rex::codegen {

/**
 * Collects a warning for each key of a binary config table that the loader does not read or that
 * has the wrong type. Nested tables and arrays of tables are checked too.
 *
 * @param root The table holding the binary config keys.
 * @param source The file and section the table came from, the start of every warning.
 * @param warnings Receives one warning per key.
 */
void CheckConfigKeys(const toml::table& root, std::string_view source,
                     std::vector<std::string>& warnings);

/**
 * Collects a warning for each key of a manifest `[project]` table that the loader does not read or
 * that has the wrong type.
 *
 * @param project The `[project]` table.
 * @param source The file and section the table came from, the start of every warning.
 * @param warnings Receives one warning per key.
 */
void CheckProjectKeys(const toml::table& project, std::string_view source,
                      std::vector<std::string>& warnings);

}  // namespace rex::codegen
