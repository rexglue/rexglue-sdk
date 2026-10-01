/**
 * @file        system/module_registry.h
 * @brief       Registry of loaded kernel and user modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmodule.h>
#include <rex/system/xtypes.h>

namespace rex::system {

/**
 * Lists the kernel modules, the loaded user modules and the executable. Every handle it stores is
 * a duplicate that it owns and closes.
 */
class ModuleRegistry {
 public:
  /**
   * @param table The table that holds the module records.
   */
  explicit ModuleRegistry(util::ObjectTable* table) : table_(table) {}

  /**
   * Adds a kernel module.
   *
   * @param handle A handle of the module.
   * @return The handle the registry owns, 0 when the handle is invalid.
   */
  X_HANDLE AddKernel(X_HANDLE handle);

  /**
   * Adds a user module unless a loaded module has the same path.
   *
   * @param handle A handle of the module.
   * @return True when the module was added.
   */
  bool AddUser(X_HANDLE handle);

  /**
   * Removes a user module and closes the handle the registry owned for it.
   *
   * @param handle Any handle of the module.
   * @return True when the module was listed.
   */
  bool RemoveUser(X_HANDLE handle);

  /**
   * Finds a kernel module.
   *
   * @param name A module name, a file name or a path.
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE FindKernel(std::string_view name);

  /**
   * Finds a loaded user module.
   *
   * @param name A module name, a file name or a path.
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE FindUser(std::string_view name);

  /**
   * Finds a kernel module, then a loaded user module.
   *
   * @param name A module name, a file name or a path.
   * @param user_only True to skip the kernel modules.
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE Find(std::string_view name, bool user_only);

  /**
   * Tests whether a name belongs to a kernel module.
   *
   * @param name A module name, a file name or a path.
   * @return True when a kernel module matches, false for an empty name.
   */
  bool IsKernel(std::string_view name);

  /**
   * Returns the executable module.
   *
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE executable() const;

  /**
   * Sets the executable module and closes the previous executable handle.
   *
   * @param handle Any handle of the module, or 0 to clear it.
   */
  void SetExecutable(X_HANDLE handle);

  /**
   * Lists the kernel modules in load order.
   *
   * @return The handles the registry owns.
   */
  std::vector<X_HANDLE> kernel_modules();

  /**
   * Lists the loaded user modules in load order.
   *
   * @return The handles the registry owns.
   */
  std::vector<X_HANDLE> user_modules();

  /**
   * Marks a path as being loaded.
   *
   * @param path The path.
   * @return False when another thread already loads the path.
   */
  bool BeginLoad(std::string_view path);

  /**
   * Clears the loading mark of a path.
   *
   * @param path The path.
   */
  void EndLoad(std::string_view path);

  /** Closes every handle the registry owns, user modules in reverse load order first. */
  void Shutdown();

 private:
  util::ObjectTable* table_;
  mutable rex::platform::RankedMutex mutex_{rex::platform::LockRank::ModuleRegistry};
  X_HANDLE executable_ = 0;
  std::vector<X_HANDLE> kernel_modules_;
  std::vector<X_HANDLE> user_modules_;
  std::unordered_set<std::string> loading_paths_;
};

}  // namespace rex::system
