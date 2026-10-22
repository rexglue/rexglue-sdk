/**
 * @file        image_info.h
 * @brief       Image layout descriptor for recompiled binaries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>

#include <rex/types.h>

struct PPCFuncMapping;

namespace rex::runtime {
class IModuleRegistry;
}  // namespace rex::runtime

namespace rex {

/**
 * Registers the recompiled libraries of a project with several binaries.
 *
 * @param modules The module registry of the runtime being launched.
 */
using RegisterModulesFunc = void (*)(runtime::IModuleRegistry& modules);

/**
 * Returns the recompiled code that codegen generated for an image.
 *
 * @return The code range.
 */
using RecompiledCodeFunc = std::span<const u8> (*)();

/** Codegen options that change how the generated code treats registers and state. */
struct PPCCodegenFlags {
  /** Omits the `ctx.lr` store at calls between guest functions and the `mflr` read. Calls to
   * imports always store `ctx.lr`. */
  bool skip_lr = false;
  /** Keeps CTR in a local variable instead of `ctx.ctr`. Config key `ctr_as_local`. */
  bool ctr_as_local = false;
  /** Keeps XER in a local variable instead of `ctx.xer`. Config key `xer_as_local`. */
  bool xer_as_local = false;
  /** Keeps the reservation register in a local variable instead of `ctx.reserved`. */
  bool reserved_as_local = false;
  /** Omits the generated global lock check that mfmsr emits. Config key `skip_msr`. */
  bool skip_msr = false;
  /** Keeps the CR fields in local variables instead of `ctx.cr0` to `ctx.cr7`. */
  bool cr_as_local = false;
  /** Keeps r0, r11, r12, f0 and v32 to v63 in local variables. */
  bool non_argument_as_local = false;
  /** Keeps r14 and above, f14 and above and the high vector registers in locals. */
  bool non_volatile_as_local = false;
};

/** The argument registers a recompiled function reads at its entry. */
struct PPCCallUsage {
  u32 address;  /**< Guest address of the function. */
  u8 gpr_mask;  /**< Bit n is set when the function reads r(3 + n), r3 to r10. */
  u16 fpr_mask; /**< Bit n is set when the function reads f(1 + n), f1 to f13. */
};

/** PPC image layout passed from the generated config header into TitleApp::Init. */
struct PPCImage {
  u32 code_base;  /**< First guest address of the generated code, given to the function table. */
  u32 code_size;  /**< Size of the generated code in bytes. */
  u32 image_base; /**< Guest address the image loads at. */
  u32 image_size; /**< Size of the image in bytes. */
  /** Guest address and host function pairs registered at startup, ended by guest address 0. */
  const PPCFuncMapping* func_mappings;
  bool shim_heap = false; /**< Set by codegen when [shim] holds the heap functions. */
  RegisterModulesFunc register_modules = nullptr; /**< Set by codegen for multi binary projects. */
  PPCCodegenFlags codegen_flags{};                /**< Set by codegen from the config flags. */
  RecompiledCodeFunc recompiled_code =
      nullptr; /**< Set by codegen to the generated recompiled code. */
  /** Set by codegen to the argument registers each function reads, ended by address 0. */
  const PPCCallUsage* call_usage = nullptr;
};

}  // namespace rex
