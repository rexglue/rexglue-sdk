/**
 * @file        rexglue/command/package_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>

namespace rexglue::cli {

/**
 * Reads the CMake user package registry and reports whether it lists an SDK install folder.
 * Nothing is written. The registry is HKCU\Software\Kitware\CMake\Packages\rexglue on Windows
 * and ~/.cmake/packages/rexglue elsewhere.
 *
 * @param prefix The install folder of the SDK.
 * @return True when an entry names the folder.
 */
[[nodiscard]] bool IsPackageRegistered(const std::filesystem::path& prefix);

}  // namespace rexglue::cli
