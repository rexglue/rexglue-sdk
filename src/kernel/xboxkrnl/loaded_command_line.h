/**
 * @file        kernel/xboxkrnl/loaded_command_line.h
 * @brief       ExLoadedCommandLine text and buffer size
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

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

namespace rex::runtime {

/**
 * Builds the ExLoadedCommandLine text, the quoted image file name followed by the arguments.
 *
 * @param image_path The path of the loaded image.
 * @param arguments The configured command line, empty for none.
 * @return The command line.
 */
REX_RUNTIME_API std::string LoadedCommandLine(std::string_view image_path,
                                              std::string_view arguments);

/**
 * Returns the ExLoadedCommandLine buffer size that holds any image name with the arguments.
 *
 * @param arguments The configured command line, empty for none.
 * @return The size in bytes, including the terminator.
 */
REX_RUNTIME_API u32 LoadedCommandLineCapacity(std::string_view arguments);

}  // namespace rex::runtime
