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

struct PPCCodegenFlags {
  bool skip_lr = false;
  bool ctr_as_local = false;
  bool xer_as_local = false;
  bool reserved_as_local = false;
  bool skip_msr = false;
  bool cr_as_local = false;
  bool non_argument_as_local = false;
  bool non_volatile_as_local = false;
};

/// PPC image layout passed from the generated config header into TitleApp::Init.
struct PPCImageInfo {
  u32 code_base;
  u32 code_size;
  u32 image_base;
  u32 image_size;
  const PPCFuncMapping* func_mappings;
  bool shim_heap = false;  ///< Set by codegen when [shim] holds the heap functions
  RegisterModulesFunc register_modules = nullptr;  ///< Set by codegen for multi-binary projects
  PPCCodegenFlags codegen_flags{};                 ///< Set by codegen from the config flags
};

}  // namespace rex
