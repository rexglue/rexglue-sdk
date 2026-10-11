/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string_view>

#include <rex/macros.h>
#include <rex/runtime/exports.h>
#include <rex/runtime/module_object.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/**
 * Creates the `KernelObject` of a kernel module with its first handle.
 *
 * @param runtime The runtime the call runs in.
 * @param path The guest path of the module, its name is the file name of the path.
 * @return The handle with a count of 1, 0 when the table cannot grow.
 */
REX_RUNTIME_API X_HANDLE CreateKernelModule(Runtime& runtime, std::string_view path);

/**
 * Erases the cached thunks whose guest address falls in a range.
 *
 * @param module A kernel module.
 * @param lo The first address of the range.
 * @param hi The address after the last address of the range.
 */
REX_RUNTIME_API void KernelModuleInvalidateThunkCacheInRange(ModuleObject& module, u32 lo, u32 hi);

/**
 * Binds a variable export to the guest address that holds it.
 *
 * @param module A kernel module.
 * @param ordinal The variable ordinal.
 * @param address The guest address of the variable.
 */
inline void KernelModuleSetVariable(ModuleObject& module, u16 ordinal, u32 address) {
  module.kernel.variables.Set(ordinal, address);
}

/**
 * Returns the address of a variable export.
 *
 * @param module A kernel module.
 * @param ordinal The variable ordinal.
 * @return The guest address, or 0 when the module never created the variable.
 */
inline u32 KernelModuleVariable(const ModuleObject& module, u16 ordinal) {
  return module.kernel.variables.Get(ordinal);
}

}  // namespace rex::runtime
