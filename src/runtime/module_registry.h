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

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/platform/dynlib.h>
#include <rex/platform/lock_order.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/module_object.h>
#include <rex/string/key.h>

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
  std::optional<X_HANDLE> GetExecutable() const override;
  std::optional<X_HANDLE> FindKernel(std::string_view name) override;
  std::optional<X_HANDLE> FindUser(std::string_view name) override;
  bool IsKernel(std::string_view name) override;
  std::vector<X_HANDLE> GetKernelModules() override;
  std::vector<X_HANDLE> GetUserModules() override;
  Result<X_HANDLE> LoadUserModule(std::string_view name, bool call_entry = true) override;
  void UnloadUserModule(X_HANDLE module, bool call_entry = true) override;
  [[nodiscard]] Result<X_HANDLE> LoadKernelModule(X_HANDLE created) override;
  void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                const char* shared_lib_name) override;
  void AddImportCandidate(std::string_view path) override;
  rex::platform::RankedMutex& GetLoadCountMutex() override { return load_count_mutex_; }

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

  /**
   * Returns the counter that changes whenever a user module is added or removed.
   *
   * @return The generation, never 0.
   */
  u64 GetGeneration() const { return generation_.load(std::memory_order_acquire); }

  /**
   * Resolves an export of a loaded user module to its recompiled function.
   *
   * @param library The base name of the module, without directory and extension.
   * @param ordinal The export ordinal.
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
  struct RecompiledModuleInfo {
    std::string pe_name;
    std::string guest_path;
    std::string shared_lib_name;
  };

  std::optional<RecompiledModuleInfo> FindRecompiledModule(std::string_view guest_path);

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

  /**
   * Writes an import slot of a loaded image, lifting the page protection around the write.
   *
   * @param slot The guest address of the slot.
   * @param value The value to store.
   */
  void PatchImportSlot(u32 slot, u32 value);

  /**
   * Adds one load reference to a loaded module.
   *
   * @param module The module.
   */
  void AcquireReference(ModuleObject& module);

  /**
   * Drops one load reference of a module and unloads it when the last one is gone.
   *
   * @param module Any handle of the module.
   * @param call_entry True to call the entry point of a DLL module for the process detach.
   */
  void ReleaseReference(X_HANDLE module, bool call_entry);

  /**
   * Drops the load references a module holds on the modules it imports from.
   *
   * @param module The module.
   * @param call_entry True to call the entry point of a DLL module for the process detach.
   */
  void ReleaseDependencies(ModuleObject& module, bool call_entry);

  Runtime& runtime_;
  ObjectTable& table_;
  mutable rex::platform::RankedMutex mutex_{rex::platform::LockRank::kModuleRegistry};
  rex::platform::RankedMutex load_count_mutex_{rex::platform::LockRank::kIdentity};
  rex::platform::RankedMutex recompiled_mutex_{rex::platform::LockRank::kLeaf};
  X_HANDLE executable_ = 0;
  std::vector<X_HANDLE> kernel_modules_;
  std::vector<X_HANDLE> user_modules_;
  std::atomic<u64> generation_{1};
  std::unordered_set<StringKeyCase> loading_paths_;
  std::vector<RecompiledModuleInfo> recompiled_modules_;
  std::vector<std::string> import_candidates_;
  std::unordered_map<std::string, rex::platform::DynamicLibrary> module_libraries_;
  std::vector<rex::platform::DynamicLibrary> deferred_unload_libraries_;
};

}  // namespace rex::runtime
