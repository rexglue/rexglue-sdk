/**
 * @file        runtime/module_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/lock_order.h>
#include <rex/result.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex::runtime {

/**
 * Lists the kernel modules, the loaded user modules and the executable, and loads and unloads user
 * modules with their recompiled libraries. Every handle it returns is a duplicate it owns, which
 * the caller must not close.
 */
REX_INTERFACE(IModuleRegistry, "rex.runtime.module_registry", 1)
 public:
  /**
   * Finds a loaded module.
   *
   * @param name A module name, a file name or a path, empty for the executable.
   * @param user_only True to skip the kernel modules.
   * @return The handle, empty when no module matches or the name is kernel32.dll.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> GetModule(std::string_view name,
                                                          bool user_only = false) = 0;

  /**
   * Returns the executable module.
   *
   * @return The handle, empty before an executable is loaded.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> GetExecutable() const = 0;

  /**
   * Finds a kernel module.
   *
   * @param name A module name, a file name or a path.
   * @return The handle, empty when no kernel module matches.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> FindKernel(std::string_view name) = 0;

  /**
   * Finds a loaded user module.
   *
   * @param name A module name, a file name or a path.
   * @return The handle, empty when no user module matches.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> FindUser(std::string_view name) = 0;

  /**
   * Reports whether a name belongs to a kernel module.
   *
   * @param name A module name, a file name or a path.
   * @return True when a kernel module matches, false for an empty name.
   */
  [[nodiscard]] virtual bool IsKernel(std::string_view name) = 0;

  /**
   * Lists the kernel modules in load order.
   *
   * @return The handles.
   */
  [[nodiscard]] virtual std::vector<X_HANDLE> GetKernelModules() = 0;

  /**
   * Lists the loaded user modules in load order.
   *
   * @return The handles.
   */
  [[nodiscard]] virtual std::vector<X_HANDLE> GetUserModules() = 0;

  /**
   * Loads a user module, or finds it when it is already loaded, and wires its recompiled library.
   * The caller holds one load reference on the returned module, released by XexUnloadImage.
   *
   * @param name A path, or a file name relative to the executable.
   * @param call_entry True to call the entry point of a DLL module for the process attach.
   * @return The handle, or the load status. X_STATUS_UNSUCCESSFUL when another thread loads the
   *         same path, the recompiled library does not match, or the entry point refused.
   */
  [[nodiscard]] virtual Result<X_HANDLE> LoadUserModule(std::string_view name,
                                                        bool call_entry = true) = 0;

  /**
   * Unloads a user module, releases its recompiled library at teardown and closes its handles.
   *
   * @param module Any handle of the module.
   * @param call_entry True to call the entry point of a DLL module for the process detach.
   */
  virtual void UnloadUserModule(X_HANDLE module, bool call_entry = true) = 0;

  /**
   * Adds a module path that import resolution tries when the importer directory has no match. The
   * path keeps its case and an import name matches its file name without regard to case.
   *
   * @param path The path of a module file.
   */
  virtual void AddImportCandidate(std::string_view path) = 0;

  /**
   * Registers a kernel module.
   *
   * @param created A handle of the module that a kernel module creator returned.
   * @return The handle the registry owns, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<X_HANDLE> LoadKernelModule(X_HANDLE created) = 0;

  /**
   * Registers the recompiled library that serves a title module path. Generated code calls it.
   *
   * @param pe_name The PE name of the module.
   * @param guest_path The guest path of the module.
   * @param shared_lib_name The host library, empty when the code is linked into the executable.
   */
  virtual void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                        const char* shared_lib_name) = 0;

  /**
   * Returns the lock that serializes module load count changes against module lookups.
   *
   * @return The lock.
   */
  [[nodiscard]] virtual rex::platform::RankedMutex& GetLoadCountMutex() = 0;
};

}  // namespace rex::runtime
