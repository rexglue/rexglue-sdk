/**
 * @file        kernel/test_access.h
 * @brief       What cross_module_tests reaches in the loaded rexkernel library (private)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

namespace rex::kernel {

class FunctionDispatcher;
class GuestAddressSpace;
class TitleRuntime;

/**
 * Returns the guest address space of a loaded runtime. It exists for cross_module_tests, which
 * links the rexkernel library and cannot reach TitleState.
 *
 * @param runtime A runtime after Load.
 * @return The address space the runtime owns.
 */
REX_KERNEL_API GuestAddressSpace& GetAddressSpace(TitleRuntime& runtime);

/**
 * Returns the function dispatcher of a loaded runtime. It exists for cross_module_tests, which
 * links the rexkernel library and cannot reach TitleState.
 *
 * @param runtime A runtime after Load.
 * @return The dispatcher the runtime owns.
 */
REX_KERNEL_API FunctionDispatcher& GetFunctionDispatcher(TitleRuntime& runtime);

/**
 * Starts the DPC queue of a runtime that was loaded and not launched. It exists for
 * cross_module_tests, which links the rexkernel library and cannot reach the dispatcher.
 *
 * @param runtime A runtime after Load.
 */
REX_KERNEL_API void StartDPCQueue(TitleRuntime& runtime);

}  // namespace rex::kernel
