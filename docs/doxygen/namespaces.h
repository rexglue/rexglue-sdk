/**
 * @file        namespaces.h
 * @brief       Namespace pages for the reference
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Documentation only. Doxygen lists the members of a namespace, such as its enumerations and
 * constants, only when the namespace has a page, and nothing includes this file.
 */

#pragma once

/** Log levels, categories, sinks and the logger. */
namespace rex::log {}

/** The graphics driver interface and the host API selection. */
namespace rex::gpu {}

/** The audio driver interface and its configuration. */
namespace rex::apu {}

/** Kernel exports, shims and the kernel initializer. */
namespace rex::kernel {}

/** The shim registry and the shim heap. */
namespace rex::kernel::shim {}

/** The PowerPC context, intrinsics and hook support that generated code uses. */
namespace rex::ppc {}

/** The vector intrinsics that generated code uses. */
namespace rex::ppc::simd {}

/** Guest memory types and helpers. */
namespace rex::memory {}

/** Host threads, events, timers and other platform primitives. */
namespace rex::platform {}

/** The recompiler that `rexglue codegen` runs. */
namespace rex::codegen {}
