/**
 * @file        memory.h
 * @brief       Memory subsystem umbrella header
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

// Memory utilities (page allocation, protection, load/store helpers)
#include <rex/memory/utils.h>

// Arena allocator
#include <rex/memory/arena.h>

// Memory-mapped files
#include <rex/memory/mapped_memory.h>

// Ring buffer
#include <rex/memory/ring_buffer.h>

// Memory class (Xbox 360 guest memory system with heaps)
#include <rex/system/xmemory.h>
