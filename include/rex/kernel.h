/**
 * @file        kernel.h
 * @brief       Umbrella header for Xbox kernel types and objects
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

// Core system types (X_STATUS, X_RESULT, X_HRESULT, X_HANDLE)
#include <rex/system/xtypes.h>

// I/O types (file attributes, overlapped)
#include <rex/system/xio.h>

// Kernel objects
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmutant.h>
#include <rex/system/xobject.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

// Memory (Xbox API wrappers - core Memory class is in rex/memory.h)
#include <rex/system/xmemory.h>

// System state
#include <rex/system/kernel_state.h>
