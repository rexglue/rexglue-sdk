/**
 * @file        test_memory.h
 * @brief       Single shared guest Memory instance for the unit test binary
 *
 * Memory::Initialize reserves a 4.5GB page-file backed mapping. Two instances
 * in one process do not both survive, so every test must share this one or
 * release it first.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <rex/system/xmemory.h>

namespace rex::testing {

/// Returns the process-wide guest Memory, initializing it on first use.
/// Fails the calling assertion if initialization did not succeed.
rex::memory::Memory& GetTestMemory();

/// Destroys the shared Memory so another owner can create its own. The next
/// GetTestMemory call creates it again.
void ReleaseTestMemory();

}  // namespace rex::testing
