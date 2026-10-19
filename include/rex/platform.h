/**
 * @file        platform.h
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
/** 1 when the target is macOS. */
#define REX_PLATFORM_MAC 1
#elif defined(WIN32) || defined(_WIN32)
/** 1 when the target is Windows. */
#define REX_PLATFORM_WIN32 1
#elif defined(__ANDROID__)
/** 1 when the target is Android. */
#define REX_PLATFORM_ANDROID 1
/** 1 when the target is Linux, Android included. */
#define REX_PLATFORM_LINUX 1
#elif defined(__gnu_linux__)
/** 1 when the target is GNU Linux. */
#define REX_PLATFORM_GNU_LINUX 1
#define REX_PLATFORM_LINUX 1
#else
#error Unsupported target OS.
#endif

/** Each REX_PLATFORM_ flag not set above is 0, so it works in #if and static_assert. */
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
/** 1 when the compiler is GCC. */
#define REX_COMPILER_GNUC 1
#endif

#if defined(_M_AMD64) || defined(__amd64__)
/** 1 when the target architecture is x86-64. */
#define REX_ARCH_AMD64 1
#elif defined(_M_ARM64) || defined(__aarch64__)
/** 1 when the target architecture is ARM64. */
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
 * @param body Declaration to pack.
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
 * @param name Struct name.
 * @param value Braced member list.
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
 * @param name Union name.
 * @param value Braced member list.
 * @code
 * REX_PACKED_UNION(Word, {
 *   u32 value;
 *   u8 bytes[4];
 * })
 * @endcode
 */
#define REX_PACKED_UNION(name, value) REX_DETAIL_PACKED_SCOPE(union name value)

namespace rex::platform {

/** True when the host is Windows. */
inline constexpr bool kWindows = REX_PLATFORM_WIN32;
/** True when the host is Linux. */
inline constexpr bool kLinux = REX_PLATFORM_LINUX;
/** True when the host is macOS. */
inline constexpr bool kMac = REX_PLATFORM_MAC;
/** True when the host is Linux or macOS. */
inline constexpr bool kPosix = kLinux || kMac;

#if REX_PLATFORM_WIN32
/** Native path separator of the host. */
inline constexpr char kPathSeparator = '\\';
#else
/** Native path separator of the host. */
inline constexpr char kPathSeparator = '/';
#endif  // REX_PLATFORM_WIN32

}  // namespace rex::platform
