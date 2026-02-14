/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

// NOTE: This file is auto-included by platform.h on Windows.
// It contains Windows-specific headers and definitions.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

#include <ObjBase.h>
#include <SDKDDKVer.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tpcshrd.h>
#undef DeleteBitmap
#undef DeleteFile
#undef GetFirstChild
