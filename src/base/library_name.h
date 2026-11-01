/**
 * @file        base/library_name.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string_view>

#include <rex/platform.h>

namespace rex::detail {

/**
 * Returns the platform file name of a shared library.
 *
 * @param name The library target name.
 */
REX_API std::filesystem::path LibraryFileName(std::string_view name);

/**
 * Returns the file name of a module, including the build configuration postfix.
 *
 * @param name The module name without postfix or extension.
 */
REX_API std::filesystem::path ModuleFileName(std::string_view name);

}  // namespace rex::detail
