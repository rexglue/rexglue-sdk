/**
 * @file        gpu/xenos/xenos_export.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

/**
 * Marks a declaration that rexgpu-xenos exports to the host API libraries. It expands to nothing
 * when REX_GPU_XENOS_STATIC is defined, to the export form while rexgpu-xenos builds, and to the
 * import form elsewhere.
 *
 * @code
 * class REX_GPU_XENOS_API CommandProcessor;
 * REX_GPU_XENOS_API void DumpShaders();
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_GPU_XENOS_STATIC)
#define REX_GPU_XENOS_API
#elif defined(REX_BUILDING_GPU_XENOS)
#define REX_GPU_XENOS_API __declspec(dllexport)
#else
#define REX_GPU_XENOS_API __declspec(dllimport)
#endif
#else
#define REX_GPU_XENOS_API __attribute__((visibility("default")))
#endif
