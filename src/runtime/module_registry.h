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
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/platform/dynlib.h>
#include <rex/platform/lock_order.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/xmodule.h>

#include "runtime/object_table.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** The module registry of a runtime. */
class ModuleRegistry final : public IModuleRegistry {
 public:
  /**
   * @param runtime The runtime whose file system, memory and function dispatcher modules use.
   * @param table The table that holds the modules.
   */
  ModuleRegistry(Runtime& runtime, ObjectTable& table);

  std::optional<X_HANDLE> GetModule(std::string_view name, bool user_only = false) override;
  std::optional<X_HANDLE> executable() const override;
  std::optional<X_HANDLE> FindKernel(std::string_view name) override;
  std::optional<X_HANDLE> FindUser(std::string_view name) override;
  bool IsKernel(std::string_view name) override;
  std::vector<X_HANDLE> kernel_modules() override;
  std::vector<X_HANDLE> user_modules() override;
  Result<X_HANDLE> LoadUserModule(std::string_view name, bool call_entry = true) override;
  void UnloadUserModule(X_HANDLE module, bool call_entry = true) override;
  [[nodiscard]] Result<X_HANDLE> LoadKernelModule(X_HANDLE created) override;
  void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                const char* shared_lib_name) override;
  rex::platform::RankedMutex& load_count_mutex() override { return load_count_mutex_; }

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
   * Sets the executable module and closes the previous executable handle.
   *
   * @param handle Any handle of the module, or 0 to clear it.
   */
  void SetExecutable(X_HANDLE handle);

  /**
   * Marks a path as being loaded.
   *
   * @param path The path.
   * @return False when another thread loads the path.
   */
  bool BeginLoad(std::string_view path);

  /**
   * Clears the loading mark of a path.
   *
   * @param path The path.
   */
  void EndLoad(const std::string& path);

  /** Closes every handle the registry owns, user modules in reverse load order first. */
  void Shutdown();

  /** Unloads every user module in reverse load order without entry calls, then closes the lists. */
  void UnloadAll();

  /** Releases the recompiled libraries unloaded since setup. Runs last at teardown. */
  void DrainLibraries();

 private:
  struct RecompiledModuleInfo {
    std::string pe_name;
    std::string guest_path;
    std::string shared_lib_name;
  };

  std::optional<RecompiledModuleInfo> FindRecompiledModule(std::string_view guest_path);

  Runtime& runtime_;
  ObjectTable& table_;
  mutable rex::platform::RankedMutex mutex_{rex::platform::LockRank::ModuleRegistry};
  rex::platform::RankedMutex load_count_mutex_{rex::platform::LockRank::Identity};
  rex::platform::RankedMutex recompiled_mutex_{rex::platform::LockRank::Leaf};
  X_HANDLE executable_ = 0;
  std::vector<X_HANDLE> kernel_modules_;
  std::vector<X_HANDLE> user_modules_;
  std::unordered_set<std::string> loading_paths_;
  std::vector<RecompiledModuleInfo> recompiled_modules_;
  std::unordered_map<std::string, rex::platform::DynamicLibrary> module_libraries_;
  std::vector<rex::platform::DynamicLibrary> deferred_unload_libraries_;
};

}  // namespace rex::runtime
