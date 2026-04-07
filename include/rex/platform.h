/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

// This file contains the main platform switches used by rex as well as any
// fixups required to normalize the environment. Everything in here should be
// largely portable.
// Platform-specific headers, like platform_win.h, are used to house any
// super platform-specific stuff that implies code is not platform-agnostic.
//
// NOTE: ordering matters here as sometimes multiple flags are defined on
// certain platforms.
//
// Great resource on predefined macros:
// https://sourceforge.net/p/predef/wiki/OperatingSystems/
// Original link: https://predef.sourceforge.net/preos.html

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

// Ensure all platform macros are always defined (0 when inactive)
// so they can be used in static_assert and regular expressions.
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

#if defined(__clang__)
#define REX_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define REX_COMPILER_GNUC 1
#elif defined(_MSC_VER)
#define REX_COMPILER_MSVC 1
#elif defined(__MINGW32)
#define REX_COMPILER_MINGW32 1
#elif defined(__INTEL_COMPILER)
#define REX_COMPILER_INTEL 1
#else
#define REX_COMPILER_UNKNOWN 1
#endif

#if defined(_M_AMD64) || defined(__amd64__)
#define REX_ARCH_AMD64 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define REX_ARCH_ARM64 1
#elif defined(_M_IX86) || defined(__i386__) || defined(_M_ARM) || defined(__arm__)
#error Rex is not supported on 32-bit platforms.
#elif defined(_M_PPC) || defined(__powerpc__)
#define REX_ARCH_PPC 1
#endif

#if REX_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // Don't want windows.h including min/max macros.
#endif            // REX_PLATFORM_WIN32

#if REX_PLATFORM_WIN32
#include <intrin.h>
#elif REX_ARCH_AMD64
#include <x86intrin.h>
#endif  // REX_PLATFORM_WIN32

#if REX_PLATFORM_MAC
#include <libkern/OSByteOrder.h>
#endif  // REX_PLATFORM_MAC

#include <bit>
#include <cstdint>

//=============================================================================
// Compiler Polyfills
//=============================================================================

#if defined(__clang__)
// Clang has builtin rotate functions and debugtrap
#elif defined(__GNUC__)
#ifndef __builtin_rotateleft32
#define __builtin_rotateleft32(x, n) std::rotl(static_cast<uint32_t>(x), static_cast<int>(n))
#endif
#ifndef __builtin_rotateleft64
#define __builtin_rotateleft64(x, n) std::rotl(static_cast<uint64_t>(x), static_cast<int>(n))
#endif
#ifndef __builtin_debugtrap
#if defined(__x86_64__) || defined(__i386__)
#define __builtin_debugtrap() __asm__ __volatile__("int3")
#else
#define __builtin_debugtrap() __builtin_trap()
#endif
#endif
#endif

#if REX_COMPILER_MSVC
#define _REXPACKEDSCOPE(body) __pragma(pack(push, 1)) body __pragma(pack(pop));
#else
#define _REXPACKEDSCOPE(body)    \
  _Pragma("pack(push, 1)") body; \
  _Pragma("pack(pop)");
#endif  // REX_COMPILER_MSVC

#define REXPACKEDSTRUCT(name, value) _REXPACKEDSCOPE(struct name value)
#define REXPACKEDSTRUCTANONYMOUS(value) _REXPACKEDSCOPE(struct value)
#define REXPACKEDUNION(name, value) _REXPACKEDSCOPE(union name value)

// Compiler capability macros
#if REX_COMPILER_CLANG || REX_COMPILER_GNUC
#define REX_HAS_BUILTIN_STRLEN 1
#define REX_LACKS_FLOAT_FROM_CHARS 1
#else
#define REX_HAS_BUILTIN_STRLEN 0
#define REX_LACKS_FLOAT_FROM_CHARS 0
#endif

namespace rex {

#if REX_PLATFORM_WIN32
const char kPathSeparator = '\\';
#else
const char kPathSeparator = '/';
#endif  // REX_PLATFORM_WIN32

const char kGuestPathSeparator = '\\';

}  // namespace rex
