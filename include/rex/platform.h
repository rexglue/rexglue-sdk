/**
 * @file        platform.h
 * @brief       Platform flags, packing macros and export markers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/platform.h (Ben Vanik, 2020).
 */

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_OS_MAC) && TARGET_OS_MAC
#define REX_PLATFORM_MAC 1
#elif defined(WIN32) || defined(_WIN32)
#define REX_PLATFORM_WIN32 1
#elif defined(__ANDROID__)
#define REX_PLATFORM_ANDROID 1
#define REX_PLATFORM_LINUX 1
#elif defined(__gnu_linux__)
#define REX_PLATFORM_GNU_LINUX 1
#define REX_PLATFORM_LINUX 1
#else
#error Unsupported target OS.
#endif

/**
 * Each REX_PLATFORM_ flag not set above is 0, so it works in preprocessor conditions and
 * static_assert.
 */
#ifndef REX_PLATFORM_MAC
#define REX_PLATFORM_MAC 0
#endif
#ifndef REX_PLATFORM_WIN32
#define REX_PLATFORM_WIN32 0
#endif
#ifndef REX_PLATFORM_ANDROID
#define REX_PLATFORM_ANDROID 0
#endif
#ifndef REX_PLATFORM_GNU_LINUX
#define REX_PLATFORM_GNU_LINUX 0
#endif
#ifndef REX_PLATFORM_LINUX
#define REX_PLATFORM_LINUX 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define REX_COMPILER_GNUC 1
#endif

#if defined(_M_AMD64) || defined(__amd64__)
#define REX_ARCH_AMD64 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define REX_ARCH_ARM64 1
#elif defined(_M_IX86) || defined(__i386__) || defined(_M_ARM) || defined(__arm__)
#error ReX is not supported on 32 bit platforms.
#endif

#if REX_PLATFORM_WIN32
#include <intrin.h>
#endif  // REX_PLATFORM_WIN32

#if REX_PLATFORM_MAC
#include <libkern/OSByteOrder.h>
#endif  // REX_PLATFORM_MAC

#include <bit>
#include <cstdint>

/**
 * Wraps a declaration in 1 byte packing.
 *
 * @code
 * REX_DETAIL_PACKED_SCOPE(struct Header { u8 tag; u32 size; })
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): body is a whole declaration
#if REX_COMPILER_MSVC
#define REX_DETAIL_PACKED_SCOPE(body) __pragma(pack(push, 1)) body __pragma(pack(pop));
#else
#define REX_DETAIL_PACKED_SCOPE(body) \
  _Pragma("pack(push, 1)") body;      \
  _Pragma("pack(pop)");
#endif  // REX_COMPILER_MSVC
// NOLINTEND(bugprone-macro-parentheses)

/**
 * Declares a struct with 1 byte packing.
 *
 * @param name An identifier, pasted after struct.
 * @param value Braced member list.
 *
 * @code
 * REX_PACKED_STRUCT(Header, {
 *   u8 tag;
 *   u32 size;
 * })
 * @endcode
 */
#define REX_PACKED_STRUCT(name, value) REX_DETAIL_PACKED_SCOPE(struct name value)
/**
 * Declares an unnamed struct with 1 byte packing.
 *
 * @param value Braced member list.
 *
 * @code
 * REX_PACKED_STRUCT_ANONYMOUS({
 *   u8 tag;
 *   u32 size;
 * })
 * @endcode
 */
#define REX_PACKED_STRUCT_ANONYMOUS(value) REX_DETAIL_PACKED_SCOPE(struct value)
/**
 * Declares a union with 1 byte packing.
 *
 * @param value Braced member list.
 *
 * @code
 * REX_PACKED_UNION(Word, {
 *   u32 value;
 *   u8 bytes[4];
 * })
 * @endcode
 */
#define REX_PACKED_UNION(name, value) REX_DETAIL_PACKED_SCOPE(union name value)

/**
 * Turns the argument into a string literal without expanding it first.
 *
 * @code
 * REX_XSTRINGIFY(__LINE__)
 * @endcode
 */
#define REX_XSTRINGIFY(x) #x
/**
 * Expands the argument, then turns it into a string literal.
 *
 * @code
 * REX_STRINGIFY(__LINE__)
 * @endcode
 */
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)

/** Makes a return statement a guaranteed tail call where the compiler supports it. */
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::musttail)
#define REX_MUSTTAIL [[clang::musttail]]
#endif
#endif
#ifndef REX_MUSTTAIL
#define REX_MUSTTAIL
#endif

/**
 * Marks a declaration that rexbase exports.
 *
 * @code
 * class REX_API Clock;
 * REX_API void Break();
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_BASE)
#define REX_API __declspec(dllexport)
#else
#define REX_API __declspec(dllimport)
#endif
#else
#define REX_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexppc exports.
 *
 * @code
 * REX_PPC_API void AttachMMIO(MMIOBlock block, const MMIORecipient& recipient);
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_PPC)
#define REX_PPC_API __declspec(dllexport)
#else
#define REX_PPC_API __declspec(dllimport)
#endif
#else
#define REX_PPC_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexapu exports.
 *
 * @code
 * REX_APU_API class AudioSettings;
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_APU)
#define REX_APU_API __declspec(dllexport)
#else
#define REX_APU_API __declspec(dllimport)
#endif
#else
#define REX_APU_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexhid exports.
 *
 * @code
 * REX_HID_API class InputSettings;
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_HID)
#define REX_HID_API __declspec(dllexport)
#else
#define REX_HID_API __declspec(dllimport)
#endif
#else
#define REX_HID_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexgpu exports.
 *
 * @code
 * REX_GPU_API class GraphicsSettings;
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_GPU)
#define REX_GPU_API __declspec(dllexport)
#else
#define REX_GPU_API __declspec(dllimport)
#endif
#else
#define REX_GPU_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexui exports.
 *
 * @code
 * REX_UI_API class UISettings;
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_UI)
#define REX_UI_API __declspec(dllexport)
#else
#define REX_UI_API __declspec(dllimport)
#endif
#else
#define REX_UI_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexapp exports.
 *
 * @code
 * class REX_APP_API App;
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_APP)
#define REX_APP_API __declspec(dllexport)
#else
#define REX_APP_API __declspec(dllimport)
#endif
#else
#define REX_APP_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexkernel exports.
 *
 * @code
 * class REX_KERNEL_API GuestAddressSpace;
 * REX_KERNEL_API std::span<const ExportEntry> Exports(ExportModule module);
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_KERNEL)
#define REX_KERNEL_API __declspec(dllexport)
#else
#define REX_KERNEL_API __declspec(dllimport)
#endif
#else
#define REX_KERNEL_API __attribute__((visibility("default")))
#endif

/**
 * Marks a declaration that rexxam exports.
 *
 * @code
 * class REX_XAM_API XAMApplet;
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_XAM)
#define REX_XAM_API __declspec(dllexport)
#else
#define REX_XAM_API __declspec(dllimport)
#endif
#else
#define REX_XAM_API __attribute__((visibility("default")))
#endif

/**
 * Marks a translation entry point definition for export. It expands to nothing outside rexkernel,
 * so a hook a consumer defines stays local to it.
 *
 * @code
 * extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(__imp__NtClose) {}
 * @endcode
 */
#if defined(REX_BUILDING_KERNEL)
#define REX_DETAIL_ENTRY_EXPORT REX_KERNEL_API
#else
#define REX_DETAIL_ENTRY_EXPORT
#endif

/**
 * Marks a C symbol that a module binary exports, such as the entry points REX_DECLARE_MODULE
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

namespace rex::platform {

inline constexpr bool kWindows = REX_PLATFORM_WIN32;
inline constexpr bool kLinux = REX_PLATFORM_LINUX;
inline constexpr bool kMac = REX_PLATFORM_MAC;
inline constexpr bool kPosix = kLinux || kMac;

#if REX_PLATFORM_WIN32
inline constexpr char kPathSeparator = '\\';
#else
inline constexpr char kPathSeparator = '/';
#endif  // REX_PLATFORM_WIN32

}  // namespace rex::platform
