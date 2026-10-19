/**
 * @file        export.h
 * @brief       The export markers of the SDK libraries and module binaries
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
 * Marks a declaration that rexcore exports. It expands to the export form while rexcore builds
 * and to the import form in every other binary.
 *
 * @code
 * class REX_API Clock;
 * REX_API void Break();
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_CORE)
#define REX_API __declspec(dllexport)
#else
#define REX_API __declspec(dllimport)
#endif
#else
#define REX_API __attribute__((visibility("default")))
#endif

/**
 * Marks a C symbol that a module binary exports, such as the 2 entry points REX_DECLARE_MODULE
 * generates.
 *
 * @code
 * extern "C" REX_DETAIL_EXPORT void rexmod_test_set_sink(ReXModTestSink sink);
 * @endcode
 */
#if REX_PLATFORM_WIN32
#define REX_DETAIL_EXPORT __declspec(dllexport)
#else
#define REX_DETAIL_EXPORT __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexruntime exports. It expands to the export form while rexruntime
 * builds and to the import form in every other binary.
 *
 * @code
 * class REX_RUNTIME_API Memory;
 * REX_RUNTIME_API std::span<const ExportEntry> Exports(ExportModule module);
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_RUNTIME)
#define REX_RUNTIME_API __declspec(dllexport)
#else
#define REX_RUNTIME_API __declspec(dllimport)
#endif
#else
#define REX_RUNTIME_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that reximgui exports. It expands to the export form while reximgui
 * builds and to the import form in every other binary.
 *
 * @code
 * class REX_IMGUI_API ImGuiDialog;
 * REX_IMGUI_API void ApplyDefaultStyle(ImGuiStyle& imgui_style, const Palette& palette,
 *                                      const Metrics& metrics);
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_IMGUI)
#define REX_IMGUI_API __declspec(dllexport)
#else
#define REX_IMGUI_API __declspec(dllimport)
#endif
#else
#define REX_IMGUI_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexgpu-xenos exports to the host API libraries. It expands to the
 * export form while rexgpu-xenos builds, to nothing in a binary that compiles the sources itself
 * and defines REX_GPU_XENOS_STATIC, and to the import form in every other binary.
 *
 * @code
 * class REX_GPU_XENOS_API CommandProcessor;
 * REX_GPU_XENOS_API std::string GetDumpShaders();
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

/**
 * Marks a translation entry point definition for export. It expands to the export form while
 * rexruntime builds and to nothing in every other binary, so a hook a consumer defines stays
 * local to the consumer.
 *
 * @code
 * extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(__imp__NtClose) {}
 * @endcode
 */
#if defined(REX_BUILDING_RUNTIME)
#define REX_DETAIL_ENTRY_EXPORT REX_RUNTIME_API
#else
#define REX_DETAIL_ENTRY_EXPORT
#endif
