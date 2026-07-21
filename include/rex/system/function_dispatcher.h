/**
 * @file        system/function_dispatcher.h
 * @brief       Guest function dispatch coordinator for recompiled code
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's runtime::Processor (Ben Vanik, 2020).
 *              Stripped of emulation-era dead code and renamed to reflect its
 *              role as a function dispatch table rather than a CPU emulator.
 */

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/memory/mapped_memory.h>
#include <rex/ppc/context.h>
#include <rex/system/export_resolver.h>
#include <rex/system/thread_state.h>
#include <rex/thread/mutex.h>

namespace rex::runtime {

// Forward declarations
class ExportResolver;
class ThreadState;

/**
 * Narrow registration interface used by generated DLLs.
 */
class IModuleRegistrar {
 public:
  /**
   * Returns false (and logs) if guest_address is outside every registered
   * function-table range.
   */
  virtual bool SetFunction(uint32_t guest_address, ::PPCFunc* func) = 0;

 protected:
  ~IModuleRegistrar() = default;
};

class FunctionDispatcher : public IModuleRegistrar {
 public:
  /**
   * Callback type for module registration functions.
   */
  using RegisterFn = void (*)(IModuleRegistrar*);

  FunctionDispatcher(memory::Memory* memory, ExportResolver* export_resolver);
  ~FunctionDispatcher();

  memory::Memory* memory() const { return memory_; }
  ExportResolver* export_resolver() const { return export_resolver_; }

  uint64_t Execute(ThreadState* thread_state, uint32_t address, uint64_t args[], size_t arg_count);
  uint64_t ExecuteInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                            size_t arg_count);

  // Shared thunk region size per module.
  static constexpr uint32_t kThunkReserveSize = 0x10000;  // 64KB

  // rexglue function table management (per-module table at IMAGE_BASE + IMAGE_SIZE)
  // Set is_entrypoint=true exactly once for the host-loaded entrypoint so
  // AllocateThunk(caller_address=0) can route to its pool.
  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                               uint32_t image_size, bool is_entrypoint = false);
  bool SetFunction(uint32_t guest_address, ::PPCFunc* func) override;
  ::PPCFunc* GetFunction(uint32_t guest_address);
  bool HasAnyFunctionTable() const { return !module_tables_.empty(); }
  /**
   * caller_address must be inside a registered module, or 0 to mean "host-
   * initiated, route to the entrypoint pool".
   */
  uint32_t AllocateThunk(::PPCFunc* func, uint32_t caller_address);

  /**
   * Returns the `code_base` of the module containing `guest_address`,
   * or 0 if no module covers that address.
   */
  uint32_t FindCallerModuleBase(uint32_t guest_address);

  /**
   * Register a module while recording guest addresses written via SetFunction.
   * `code_base` must equal the value previously passed to InitializeFunctionTable
   * for the same module.
   */
  void RegisterModule(const std::string& module_id, uint32_t code_base, RegisterFn register_func);

  /**
   * Unregister `module_id`: clears its function-table entries, releases its
   * thunk pool, and removes its per-module function table. Returns the
   * cleared thunk-pool range `[lo, hi)` for external cache invalidation, or
   * nullopt if the module was not registered.
   */
  std::optional<std::pair<uint32_t, uint32_t>> UnregisterModule(const std::string& module_id);

 private:
  bool Execute(ThreadState* thread_state, uint32_t address);

  struct ModuleTableInfo {
    uint32_t code_base;
    uint32_t code_size;
    uint32_t image_base;
    uint32_t image_size;
    uint32_t next_thunk_address;
    uint32_t thunk_limit;
  };

  ModuleTableInfo* FindModuleByAddress(uint32_t guest_address);

  memory::Memory* memory_ = nullptr;
  ExportResolver* export_resolver_ = nullptr;

  rex::thread::global_critical_region global_critical_region_;

  // Host-side function lookup.
  std::unordered_map<uint32_t, ::PPCFunc*> function_table_;

  // Per-module function table metadata.
  std::vector<ModuleTableInfo> module_tables_;

  // code_base of the entrypoint module, or 0 if not yet registered.
  uint32_t entrypoint_code_base_ = 0;

  // Module recording for RegisterModule/UnregisterModule.
  bool recording_ = false;
  std::vector<uint32_t> recording_addresses_;

  struct ModuleRegistration {
    uint32_t code_base;
    std::vector<uint32_t> addresses;
  };

  // Recorded state per module, keyed by module_id.
  std::unordered_map<std::string, ModuleRegistration> module_addresses_;

  // Protects dispatcher metadata during module registration and callback dispatch.
  mutable std::recursive_mutex dispatch_mutex_;
};

}  // namespace rex::runtime
