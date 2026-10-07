/**
 * @file        kernel.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

// Core system types (X_STATUS, X_RESULT, X_HRESULT, X_HANDLE)
#include <rex/runtime/xtypes.h>

// I/O types (file attributes, overlapped)
#include <rex/runtime/xio.h>

// Kernel objects
#include <rex/runtime/xevent.h>
#include <rex/runtime/xfile.h>
#include <rex/runtime/xmutant.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xsemaphore.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtimer.h>

// Memory (API wrappers - core Memory class is in rex/memory.h)
#include <rex/runtime/xmemory.h>

// System state
#include <rex/runtime.h>
