/**
 * @file        kernel/function_dispatcher.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's kernel::Processor (Ben Vanik, 2020).
 */

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform/lock_order.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/thread_state.h>
#include <rex/utility.h>

namespace rex::kernel {

class ThreadState;

class IFunctionRegistrar : public NonCopyable {
 public:
  virtual ~IFunctionRegistrar() = default;

  /**
   * @return False, with a log line, when the address is outside every registered function table.
   */
  virtual bool SetFunction(u32 guest_address, ::PPCFunc* func) = 0;
};

/** Maps guest addresses to recompiled functions and runs them on guest threads. */
class REX_KERNEL_API FunctionDispatcher : public IFunctionRegistrar {
 public:
  using RegisterFn = void (*)(IFunctionRegistrar*);

  /** Builds a dispatcher with no modules. */
  explicit FunctionDispatcher(kernel::Memory* memory);
  /** Removes the recompiled code of every registered module. */
  virtual ~FunctionDispatcher();

  kernel::Memory* GetMemory() const { return memory_; }

  /**
   * Runs recompiled code on a thread and restores its register file afterwards.
   *
   * @return The value in r3 when the call returns, or 0xDEADBABE when no function is set there.
   */
  u64 Execute(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);
  /**
   * Runs recompiled code as a DPC level interrupt, then queued DPCs when the outermost ends.
   *
   * @param args The call arguments, at most 5.
   * @return The value in r3 when the call returns, or 0xDEADBABE when no function is set there.
   */
  u64 ExecuteInterrupt(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);

  /**
   * Sets the graphics interrupt callback the title registered.
   *
   * @param callback The guest address of the routine, 0 to clear it.
   */
  void SetGraphicsInterrupt(u32 callback, u32 user_data);

  /**
   * Runs the graphics interrupt callback on the calling thread, if the title set one.
   *
   * @param cpu The guest CPU the interrupt is for, 0xFFFFFFFF for CPU 2.
   */
  void RaiseGraphicsInterrupt(u32 source, u32 cpu);

  /**
   * Runs recompiled code like a trap frame APC delivery, then restores the register state.
   *
   * @return The value in r3 when the call returns, or 0xDEADBABE when it could not run.
   */
  u64 ExecuteTrap(ThreadState* thread_state, u32 address, u64 args[], size_t arg_count);

  /** Bytes of thunk region reserved per module. */
  static constexpr u32 kThunkReserveSize = 0x10000;

  /**
   * Initializes the function table of a module at image_base + image_size.
   *
   * AllocateThunk with caller_address 0 uses the pool of the entrypoint module.
   *
   * @param code_base The first guest address of the code.
   * @param is_entrypoint True for the host loaded entrypoint module.
   */
  bool InitializeFunctionTable(u32 code_base, u32 code_size, u32 image_base, u32 image_size,
                               bool is_entrypoint = false);
  /**
   * @return False when the address is outside every registered module.
   */
  bool SetFunction(u32 guest_address, ::PPCFunc* func) override;
  /** Returns the function at a guest address, null when none is set. */
  ::PPCFunc* GetFunction(u32 guest_address);
  /**
   * Allocates a thunk in the pool of the calling module.
   *
   * @param func The function the thunk calls.
   * @param caller_address An address inside a registered module, or 0 for the entrypoint pool.
   */
  u32 AllocateThunk(::PPCFunc* func, u32 caller_address);

  /**
   * Returns the code base of the module containing an address, or 0 when no module covers the
   * address.
   */
  u32 FindCallerModuleBase(u32 guest_address);

  /**
   * Registers a module and tracks the guest addresses it sets through SetFunction.
   *
   * @param code_base The code base passed to InitializeFunctionTable for the same module.
   * @param recompiled_code The recompiled code of the module, where a stop may redirect a thread.
   */
  void RegisterModule(const std::string& module_id, u32 code_base, RegisterFn register_func,
                      std::span<const u8> recompiled_code = {});

  /** Registers recompiled code, where a stop may redirect a thread. */
  void AddRecompiledCode(std::span<const u8> code);

  /**
   * Unregisters a module and clears its function table and thunk pool.
   *
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

  kernel::Memory* memory_ = nullptr;

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
  std::atomic<u64> graphics_interrupt_{0};

  rex::platform::RankedMutex interrupt_mutex_{rex::platform::lock_rank::kInterruptState};

  mutable std::mutex recompiled_code_mutex_;
  std::vector<std::pair<std::string, std::span<const u8>>> recompiled_code_;
};

}  // namespace rex::kernel
