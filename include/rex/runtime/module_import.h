/**
 * @file        runtime/module_import.h
 * @brief       Cross module import of a recompiled function
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

namespace rex::runtime {

/** A function another loaded module exports by ordinal, resolved on its first call. */
struct ModuleImport {
  /**
   * @param library_name The base name of the exporting module, without directory and extension.
   * @param export_ordinal The export ordinal.
   */
  constexpr ModuleImport(const char* library_name, u16 export_ordinal)
      : library(library_name), ordinal(export_ordinal) {}

  const char* library;                     /**< Base name of the exporting module. */
  u16 ordinal;                             /**< Export ordinal. */
  std::atomic<PPCFunc*> function{nullptr}; /**< Function resolved at generation. */
  std::atomic<u64> generation{0};          /**< Registry generation the function was resolved at. */
};

/**
 * Resolves an import to the exporting module's function, again after a module was loaded or
 * unloaded since the last call. Stops the title when the module or the export is gone.
 *
 * @param target The import to resolve.
 * @return The function.
 */
REX_RUNTIME_API PPCFunc* ResolveModuleImport(ModuleImport& target);

}  // namespace rex::runtime
