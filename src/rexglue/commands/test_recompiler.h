/**
 * @file        rexglue/commands/test_recompiler.h
 * @brief       Recompiler test command interface
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

namespace rexglue::commands {

/// Recompile PPC test binary files and generate Catch2 test cases
/// @param binDirPath Directory containing linked .bin files and .map symbol files
/// @param asmDirPath Directory containing .s source files with test specs
/// @param outDirPath Output directory for generated C++ files
/// @return true on success
bool recompile_tests(const std::string_view& binDirPath, const std::string_view& asmDirPath,
                     const std::string_view& outDirPath);

}  // namespace rexglue::commands
