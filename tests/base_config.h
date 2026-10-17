/**
 * @file        tests/base_config.h
 * @brief       A Config that reads no config file
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/api.h>

namespace rex::test {

/**
 * Builds a Config with no config file path.
 *
 * @return The Config.
 */
inline Config BaseConfig() {
  Config config;
  config.paths.config_file.clear();
  return config;
}

}  // namespace rex::test
