/**
 * @file        runtime/function_dispatcher.h
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
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/memory.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform/lock_order.h>
#include <rex/ppc/func.h>
#include <rex/runtime/thread_state.h>
#include <rex/module.h>

namespace rex::runtime {

class ThreadState;

/**
 * Registers recompiled functions by guest address.
 */
REX_INTERFACE(IModuleRegistrar)
 public:
  /**
   * Sets the function at a guest address.
   *
   * @param guest_address The guest address of the function.
   * @param func The function.
   * @return False, with a log line, when the address is outside every registered function table.
   */
  virtual bool SetFunction(u32 guest_address, ::PPCFunc* func) = 0;
};

/** Maps guest addresses to recompiled functions and runs them on guest threads. */
class REX_RUNTIME_API FunctionDispatcher : public IModuleRegistrar {
 public:
  /**
   * Callback type for module registration functions.
   */
  using RegisterFn = void (*)(IModuleRegistrar*);

  /**
   * Builds a dispatcher with no modules.
   *
   * @param memory The guest memory.
   */
  explicit FunctionDispatcher(runtime::Memory* memory);
  /** Removes the recompiled code of every registered module. */
  virtual ~FunctionDispatcher();

  /**
   * Returns the guest memory.
   *
   * @return The memory given at construction.
   */
  runtime::Memory* GetMemory() const { return memory_; }

  /**
   * Runs recompiled code on a thread and restores its register file afterwards.
   *
   * @param thread_state The thread that runs the call.
   * @param address The guest address to call.
   * @param args The call arguments.
   * @param arg_count The number of arguments.
   * @return The value in r3 when the call returns, or 0xDEADBABE when no function is set there.
   */
  u64 Execute(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);
  /**
   * Runs recompiled code as an interrupt at the DPC level, then runs the queued DPCs once the
   * outermost interrupt ends.
   *
   * @param thread_state The thread that runs the call.
   * @param address The guest address to call.
   * @param args The call arguments, at most 5.
   * @param arg_count The number of arguments.
   * @return The value in r3 when the call returns, or 0xDEADBABE when no function is set there.
   */
  u64 ExecuteInterrupt(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);

  /**
   * Executes recompiled code on a thread running recompiled code, then restores
   * its register state, matching the console kernel's trap frame APC delivery.
   *
   * @param thread_state The thread running recompiled code.
   * @param address The guest address to call.
   * @param args The call arguments.
   * @param arg_count The number of arguments.
   * @return The value in r3 when the call returns, or 0xDEADBABE when it could not run.
   */
  u64 ExecuteTrap(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);

  /** Bytes of thunk region reserved per module. */
  static constexpr u32 kThunkReserveSize = 0x10000;

  /**
   * Initializes the function table of a module at image_base + image_size.
   *
   * Exactly 1 module is the entrypoint, and AllocateThunk with caller_address 0 routes to its pool.
   *
   * @param code_base The first guest address of the code.
   * @param code_size The code size in bytes.
   * @param image_base The guest address of the image.
   * @param image_size The image size in bytes.
   * @param is_entrypoint True for the host loaded entrypoint module.
   * @return True on success.
   */
  bool InitializeFunctionTable(u32 code_base, u32 code_size, u32 image_base, u32 image_size,
                               bool is_entrypoint = false);
  /**
   * Sets the function at a guest address.
   *
   * @param guest_address The guest address of the function.
   * @param func The function.
   * @return False when the address is outside every registered module.
   */
  bool SetFunction(u32 guest_address, ::PPCFunc* func) override;
  /**
   * Returns the function at a guest address.
   *
   * @param guest_address The guest address.
   * @return The function, null when none is set.
   */
  ::PPCFunc* GetFunction(u32 guest_address);
  /**
   * Allocates a thunk in the pool of the calling module.
   *
   * @param func The function the thunk calls.
   * @param caller_address An address inside a registered module, or 0 for the entrypoint pool.
   * @return The guest address of the thunk.
   */
  u32 AllocateThunk(::PPCFunc* func, u32 caller_address);

  /**
   * Returns the code base of the module containing an address.
   *
   * @param guest_address The guest address.
   * @return The code base, or 0 when no module covers the address.
   */
  u32 FindCallerModuleBase(u32 guest_address);

  /**
   * Registers a module and tracks the guest addresses it sets through SetFunction.
   *
   * @param module_id The module name.
   * @param code_base The code base passed to InitializeFunctionTable for the same module.
   * @param register_func The generated registration function.
   * @param recompiled_code The recompiled code of the module, where a stop may redirect a thread.
   */
  void RegisterModule(const std::string& module_id, u32 code_base, RegisterFn register_func,
                      std::span<const u8> recompiled_code = {});

  /**
   * Registers recompiled code, where a stop may redirect a thread.
   *
   * @param code The recompiled code of 1 image.
   */
  void AddRecompiledCode(std::span<const u8> code);

  /**
   * Unregisters a module, clearing its function table entries, its thunk pool and its function
   * table.
   *
   * @param module_id The module name.
   * @return The cleared thunk pool range `[lo, hi)` for cache invalidation, or nullopt when the
   * module was not registered.
   */
  std::optional<std::pair<u32, u32>> UnregisterModule(const std::string& module_id);

 private:
  bool Run(ThreadState* thread_state, u32 address, std::span<const u64> args);
  u64 RunInterrupt(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);

  struct ModuleTable {
    u32 code_base;
    u32 code_size;
    u32 image_base;
    u32 image_size;
    u32 next_thunk_address;
    u32 thunk_limit;
  };

  ModuleTable* FindModuleByAddress(u32 guest_address);

  runtime::Memory* memory_ = nullptr;

  std::unordered_map<u32, ::PPCFunc*> function_table_;

  std::vector<ModuleTable> module_tables_;

  /** Code base of the entrypoint module, 0 until registered. */
  u32 entrypoint_code_base_ = 0;

  bool registering_ = false;
  std::vector<u32> registered_addresses_;

  struct ModuleRegistration {
    u32 code_base;
    std::vector<u32> addresses;
  };

  std::unordered_map<std::string, ModuleRegistration> module_addresses_;

  /** Guards module registration and callback dispatch. */
  mutable std::recursive_mutex dispatch_mutex_;

  rex::platform::RankedMutex interrupt_mutex_{rex::platform::LockRank::kInterrupt};

  mutable std::mutex recompiled_code_mutex_;
  std::vector<std::pair<std::string, std::span<const u8>>> recompiled_code_;
};

}  // namespace rex::runtime
