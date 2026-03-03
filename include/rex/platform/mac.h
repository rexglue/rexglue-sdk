/**
 ******************************************************************************
 * ReXGlue SDK - Xbox 360 Recompilation Toolkit                              *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

// NOTE: This file is auto-included by platform.h on macOS.
// It contains macOS-specific headers and definitions.

#include <cstddef>
#include <sys/types.h>

// Mach VM APIs for precise memory control on macOS
#include <mach/mach.h>
#include <mach/vm_map.h>

// macOS does not have these Linux-specific LFS64 functions.
// On macOS/Darwin, off_t is always 64-bit, so the standard
// POSIX functions are already 64-bit capable.
#ifndef __APPLE_COMPAT_LFS64_DEFINED
#define __APPLE_COMPAT_LFS64_DEFINED
#define ftruncate64 ftruncate
#define mmap64 mmap
#define fstat64 fstat
#define stat64 stat
typedef off_t off64_t;
#endif

// MAP_ANONYMOUS may be defined as MAP_ANON on some macOS versions
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
