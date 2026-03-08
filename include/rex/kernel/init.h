/**
 * @file        kernel/init.h
 * @brief       Kernel initialization entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex {
class Runtime;
namespace system {
class KernelState;
}
}  // namespace rex

namespace rex::kernel {
void InitializeKernel(Runtime* runtime, system::KernelState* kernel_state);
}
