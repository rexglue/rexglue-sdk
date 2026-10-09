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

#include <rex/macros.h>
#include <rex/memory.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform/lock_order.h>
#include <rex/ppc/func.h>
#include <rex/runtime/thread_state.h>

namespace rex::runtime {

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

  runtime::Memory* memory() const { return memory_; }

  /**
   * Runs recompiled code on a thread and restores its register file afterwards.
   *
   * @param thread_state The thread that runs the call.
   * @param address The guest address to call.
   * @param args The call arguments.
   * @param arg_count The number of arguments.
   * @return The value in r3 when the call returns, or 0xDEADBABE when no function is set there.
   */
  uint64_t Execute(ThreadState* thread_state, uint32_t address, uint64_t args[], size_t arg_count);
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
  uint64_t ExecuteInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                            size_t arg_count);

  /**
   * Executes recompiled code on a thread already running recompiled code, then restores
   * its full register state, matching the console kernel's trap-frame APC delivery.
   */
  uint64_t ExecuteTrap(ThreadState* thread_state, uint32_t address, uint64_t args[],
                       size_t arg_count);

  /** Bytes of thunk region reserved per module. */
  static constexpr uint32_t kThunkReserveSize = 0x10000;

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
  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                               uint32_t image_size, bool is_entrypoint = false);
  /**
   * Sets the function at a guest address.
   *
   * @param guest_address The guest address of the function.
   * @param func The function.
   * @return False when the address is outside every registered module.
   */
  bool SetFunction(uint32_t guest_address, ::PPCFunc* func) override;
  /**
   * Looks up the function at a guest address.
   *
   * @param guest_address The guest address.
   * @return The function, null when none is set.
   */
  ::PPCFunc* GetFunction(uint32_t guest_address);
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
   * Register a module while tracking guest addresses written via SetFunction.
   * `code_base` must equal the value previously passed to InitializeFunctionTable
   * for the same module.
   */
  void RegisterModule(const std::string& module_id, uint32_t code_base, RegisterFn register_func,
                      std::span<const u8> recompiled_code = {});

  /**
   * Registers recompiled code, where a stop may redirect a thread.
   *
   * @param code The recompiled code of 1 image.
   */
  void AddRecompiledCode(std::span<const u8> code);

  /**
   * Unregister `module_id`: clears its function-table entries, releases its
   * thunk pool, and removes its per-module function table. Returns the
   * cleared thunk-pool range `[lo, hi)` for external cache invalidation, or
   * nullopt if the module was not registered.
   */
  std::optional<std::pair<uint32_t, uint32_t>> UnregisterModule(const std::string& module_id);

 private:
  bool Run(ThreadState* thread_state, uint32_t address, std::span<const uint64_t> args);
  uint64_t RunInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                        size_t arg_count);

  struct ModuleTableInfo {
    uint32_t code_base;
    uint32_t code_size;
    uint32_t image_base;
    uint32_t image_size;
    uint32_t next_thunk_address;
    uint32_t thunk_limit;
  };

  ModuleTableInfo* FindModuleByAddress(uint32_t guest_address);

  runtime::Memory* memory_ = nullptr;

  std::unordered_map<uint32_t, ::PPCFunc*> function_table_;

  std::vector<ModuleTableInfo> module_tables_;

  /** Code base of the entrypoint module, 0 until registered. */
  uint32_t entrypoint_code_base_ = 0;

  bool recording_ = false;
  std::vector<uint32_t> recording_addresses_;

  struct ModuleRegistration {
    uint32_t code_base;
    std::vector<uint32_t> addresses;
  };

  std::unordered_map<std::string, ModuleRegistration> module_addresses_;

  /** Guards module registration and callback dispatch. */
  mutable std::recursive_mutex dispatch_mutex_;

  rex::platform::RankedMutex interrupt_mutex_{rex::platform::LockRank::Interrupt};

  mutable std::mutex recompiled_code_mutex_;
  std::vector<std::pair<std::string, std::span<const u8>>> recompiled_code_;
};

}  // namespace rex::runtime
