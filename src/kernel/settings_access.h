/**
 * @file        kernel/settings_access.h
 * @brief       Process wide settings for code inside rexkernel
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/runtime_settings.h>

namespace rex::kernel {

/** Returns the title path settings of scope "title". */
REX_KERNEL_API TitlePaths& GetTitlePaths();

/** Returns the runtime settings of scope "runtime". */
RuntimeSettings& GetRuntimeSettings();

}  // namespace rex::kernel
