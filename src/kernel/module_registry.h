/**
 * @file        kernel/module_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/platform/dynlib.h>
#include <rex/platform/lock_order.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/module_object.h>
#include <rex/string/text.h>

#include "kernel/object_table.h"

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

class ModuleRegistry final : public IModuleRegistry {
 public:
  /**
   * @param runtime The runtime whose file system, memory and function dispatcher modules use.
   * @param table The table that holds the modules.
   */
  ModuleRegistry(TitleRuntime& runtime, ObjectTable& table);

  std::optional<X_HANDLE> GetModule(std::string_view name, bool user_only = false) override;
  std::optional<X_HANDLE> GetExecutable() const override;
  std::optional<X_HANDLE> FindKernelModule(std::string_view name) override;
  std::optional<X_HANDLE> FindUserModule(std::string_view name) override;
  bool IsKernelModule(std::string_view name) override;
  std::vector<X_HANDLE> GetKernelModules() override;
  std::vector<X_HANDLE> GetUserModules() override;
  Result<X_HANDLE> LoadUserModule(std::string_view name, bool call_entry = true) override;
  void UnloadUserModule(X_HANDLE module, bool call_entry = true) override;
  [[nodiscard]] Result<X_HANDLE> RegisterKernelModule(X_HANDLE created) override;
  void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                const char* shared_lib_name) override;
  void AddImportCandidate(std::string_view path) override;
  rex::platform::RankedMutex& GetLoadCountMutex() override { return load_count_mutex_; }

  /**
   * Adds a kernel module.
   *
   * @return The handle the registry owns, 0 when the handle is invalid.
   */
  X_HANDLE AddKernel(X_HANDLE handle);

  /** Adds a user module unless a loaded module has the same path. */
  bool AddUser(X_HANDLE handle);

  /**
   * Removes a user module and closes the handle the registry owned for it.
   *
   * @param handle Any handle of the module.
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
   * @return True when the path was marked, false when another thread already loads it.
   */
  bool BeginLoad(std::string_view path);

  /** Clears the loading mark of a path. */
  void EndLoad(const std::string& path);

  /** Returns the counter that changes whenever a user module is added or removed, never 0. */
  u64 GetGeneration() const { return generation_.load(std::memory_order_acquire); }

  /**
   * Resolves an export of a loaded user module to its recompiled function.
   *
   * @return The function, or null when the module is not loaded or the export has no function.
   */
  PPCFunc* ResolveExport(std::string_view library, u16 ordinal);

  /** Closes every handle the registry owns, user modules in reverse load order first. */
  void Shutdown();

  /** Unloads every user module in reverse load order without entry calls, then closes the lists. */
  void UnloadAll();

  /** Releases the recompiled libraries unloaded since setup. Runs last at teardown. */
  void DrainLibraries();

 private:
  struct RecompiledModule {
    std::string pe_name;
    std::string guest_path;
    std::string shared_lib_name;
  };

  std::optional<RecompiledModule> FindRecompiledModule(std::string_view guest_path);

  /**
   * Finds or loads the module that serves an import library of another module.
   *
   * @param importer The module that imports the library.
   * @param importer_path The path the importer was loaded from.
   * @param library The import library.
   * @param call_entry True to call the entry point of a DLL module the import loads.
   * @return The handle, or the status of the failed load. X_STATUS_DLL_NOT_FOUND when no loaded
   *         module or file matches.
   */
  Result<X_HANDLE> FindImportLibrary(const ModuleObject& importer, std::string_view importer_path,
                                     const XEXModule::ImportLibrary& library, bool call_entry);

  /**
   * Loads the user modules a module imports from, checks every imported ordinal and patches the
   * import slots of the module with the exported addresses.
   *
   * @param module The module being loaded.
   * @param path The path the module was loaded from.
   * @param call_entry True to call the entry point of the DLL modules the imports load.
   * @return X_STATUS_SUCCESS, X_STATUS_DLL_NOT_FOUND, X_STATUS_DRIVER_ORDINAL_NOT_FOUND or the
   *         status of a failed dependency load.
   */
  X_STATUS ResolveImports(ModuleObject& module, std::string_view path, bool call_entry);

  void PatchImportSlot(u32 slot, u32 value);

  void AcquireReference(ModuleObject& module);

  void ReleaseReference(X_HANDLE module, bool call_entry);

  void ReleaseDependencies(ModuleObject& module, bool call_entry);

  TitleRuntime& runtime_;
  ObjectTable& table_;
  mutable rex::platform::RankedMutex mutex_{rex::platform::lock_rank::kModuleRegistry};
  rex::platform::RankedMutex load_count_mutex_{rex::platform::lock_rank::kObjectIdentityMap};
  rex::platform::RankedMutex recompiled_mutex_{rex::platform::LockRank::kLeaf};
  X_HANDLE executable_ = 0;
  std::vector<X_HANDLE> kernel_modules_;
  std::vector<X_HANDLE> user_modules_;
  std::atomic<u64> generation_{1};
  std::unordered_set<string::StringKeyIgnoreCase> loading_paths_;
  std::vector<RecompiledModule> recompiled_modules_;
  std::vector<std::string> import_candidates_;
  std::unordered_map<std::string, rex::platform::DynamicLibrary> module_libraries_;
  std::vector<rex::platform::DynamicLibrary> deferred_unload_libraries_;
};

}  // namespace rex::kernel
