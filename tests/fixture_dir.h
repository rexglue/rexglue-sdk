/**
 * @file        tests/fixture_dir.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>

namespace rex::test {

inline const std::filesystem::path& GetFixtureFolder() {
  static const std::filesystem::path kFolder = REXGLUE_FIXTURE_DIR;
  return kFolder;
}

}  // namespace rex::test
