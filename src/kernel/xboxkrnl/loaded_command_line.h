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
#include <rex/types.h>

namespace rex::kernel {

/** Builds the ExLoadedCommandLine text, the quoted image file name followed by the arguments. */
std::string LoadedCommandLine(std::string_view image_path, std::string_view arguments);

/** Returns the ExLoadedCommandLine buffer size in bytes, with the terminator. */
u32 LoadedCommandLineCapacity(std::string_view arguments);

}  // namespace rex::kernel
