/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/memory.h>
#include <rex/memory/mapped_memory.h>
#include <rex/ppc/context.h>
#include <rex/system/export_resolver.h>
#include <rex/system/module.h>
#include <rex/system/thread_state.h>
#include <rex/thread/mutex.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::runtime {

// Forward declarations
class ExportResolver;
class ThreadState;
class Thread;

constexpr memory::fourcc_t kProcessorSaveSignature = memory::make_fourcc("PROC");

class XexModule;

enum class Irql : uint32_t {
  PASSIVE = 0,
  APC = 1,
  DISPATCH = 2,
  DPC = 3,
};

// Describes the current state of the emulator as known to the debugger.
// This determines which state the debugger is in and what operations are
// allowed.
enum class ExecutionState {
  // Target is running; the debugger is not waiting for any events.
  kRunning,
  // Target is running in stepping mode with the debugger waiting for feedback.
  kStepping,
  // Target is paused for debugging.
  kPaused,
  // Target has been stopped and cannot be restarted (crash, etc).
  kEnded,
};

class Processor {
 public:
  Processor(memory::Memory* memory, ExportResolver* export_resolver);
  ~Processor();

  memory::Memory* memory() const { return memory_; }
  ExportResolver* export_resolver() const { return export_resolver_; }

  // Runs any pre-launch logic once the module and thread have been setup.
  void PreLaunch();

  // The current execution state of the emulator.
  ExecutionState execution_state() const { return execution_state_; }

  bool AddModule(std::unique_ptr<Module> module);
  Module* GetModule(const std::string_view name);
  std::vector<Module*> GetModules();

  bool Execute(ThreadState* thread_state, uint32_t address);
  bool ExecuteRaw(ThreadState* thread_state, uint32_t address);
  uint64_t Execute(ThreadState* thread_state, uint32_t address, uint64_t args[], size_t arg_count);
  uint64_t ExecuteInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                            size_t arg_count);

  Irql RaiseIrql(Irql new_value);
  void LowerIrql(Irql old_value);
  Irql current_irql() const { return static_cast<Irql>(irql_.load(std::memory_order_acquire)); }

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

 public:
  // TODO(benvanik): hide.
  void OnThreadCreated(uint32_t handle, ThreadState* thread_state, Thread* thread);
  void OnThreadExit(uint32_t thread_id);
  void OnThreadDestroyed(uint32_t thread_id);

  // rexglue function table management
  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                               uint32_t image_size);
  void SetFunction(uint32_t guest_address, ::PPCFunc* func);
  ::PPCFunc* GetFunction(uint32_t guest_address);
  bool HasFunctionTable() const { return function_table_initialized_; }

 private:
  memory::Memory* memory_ = nullptr;
  ExportResolver* export_resolver_ = nullptr;

  rex::thread::global_critical_region global_critical_region_;
  ExecutionState execution_state_ = ExecutionState::kPaused;
  std::vector<std::unique_ptr<Module>> modules_;

  std::atomic<uint32_t> irql_{static_cast<uint32_t>(Irql::PASSIVE)};

  // rexglue function table
  std::unordered_map<uint32_t, ::PPCFunc*> function_table_;
  uint32_t code_base_ = 0;
  uint32_t code_size_ = 0;
  uint32_t image_base_ = 0;
  uint32_t image_size_ = 0;
  bool function_table_initialized_ = false;
};

}  // namespace rex::runtime
