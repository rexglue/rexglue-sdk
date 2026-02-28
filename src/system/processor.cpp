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

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/stream.h>
#include <rex/system/export_resolver.h>
#include <rex/system/module.h>
#include <rex/system/processor.h>
#include <rex/system/thread.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/thread/atomic.h>
#include <rex/types.h>

REXCVAR_DEFINE_BOOL(disable_global_lock, false, "Runtime", "Disable global threading lock");

namespace rex::runtime {

Processor::Processor(rex::memory::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {
  // NOTE(tomc): No builtin module needed - all functions are pre-compiled
}

Processor::~Processor() {
  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
  }
}

void Processor::PreLaunch() {
  // Start running.
  execution_state_ = ExecutionState::kRunning;
  REXLOG_INFO("Processor::PreLaunch - starting execution");
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  modules_.push_back(std::move(module));
  return true;
}

Module* Processor::GetModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Module*> clone(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // rexglue: Look up pre-compiled function
  auto fn = GetFunction(address);
  if (!fn) {
    REXCPU_ERROR("Execute({:08X}): function not in function table", address);
    return false;
  }

  auto* ctx = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  ctx->r1.u64 -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = ctx->lr;
  ctx->lr = 0xBCBCBCBC;

  // NOTE(tomc): rexglue direct function call
  fn(*ctx, memory_->virtual_membase());

  ctx->lr = previous_lr;
  ctx->r1.u64 += 64 + 112;

  return true;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // tomc: Look up pre-compiled function
  auto fn = GetFunction(address);
  if (!fn) {
    REXCPU_INFO("ExecuteRaw({:08X}): address not found in function table", address);
    return false;
  }

  auto* ctx = thread_state->context();

  // NOTE(tomc): rexglue direct function call (no stack padding for raw execute)
  fn(*ctx, memory_->virtual_membase());

  return true;
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
                            size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto* ctx = thread_state->context();

  // Set up arguments (rexglue uses named registers)
  if (arg_count > 0)
    ctx->r3.u64 = args[0];
  if (arg_count > 1)
    ctx->r4.u64 = args[1];
  if (arg_count > 2)
    ctx->r5.u64 = args[2];
  if (arg_count > 3)
    ctx->r6.u64 = args[3];
  if (arg_count > 4)
    ctx->r7.u64 = args[4];
  if (arg_count > 5)
    ctx->r8.u64 = args[5];
  if (arg_count > 6)
    ctx->r9.u64 = args[6];
  if (arg_count > 7)
    ctx->r10.u64 = args[7];

  if (arg_count > 8) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory_->TranslateVirtual(static_cast<uint32_t>(ctx->r1.u64) + 0x54 - (64 + 112));
    for (size_t i = 8; i < arg_count; i++) {
      memory::store_and_swap<uint32_t>(stack_arg_base + ((i - 8) * 8),
                                       static_cast<uint32_t>(args[i]));
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return ctx->r3.u64;
}

uint64_t Processor::ExecuteInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                                     size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  // Hold the global lock during interrupt dispatch.
  // This will block if any code is in a critical region (has interrupts
  // disabled) or if any other interrupt is executing.
  auto global_lock = global_critical_region_.Acquire();

  auto* ctx = thread_state->context();
  assert_true(arg_count <= 5);

  // Set up arguments (rexglue uses named registers)
  if (arg_count > 0)
    ctx->r3.u64 = args[0];
  if (arg_count > 1)
    ctx->r4.u64 = args[1];
  if (arg_count > 2)
    ctx->r5.u64 = args[2];
  if (arg_count > 3)
    ctx->r6.u64 = args[3];
  if (arg_count > 4)
    ctx->r7.u64 = args[4];

  // TLS ptr must be zero during interrupts. Some games check this and
  // early-exit routines when under interrupts.
  auto pcr_address = memory_->TranslateVirtual(static_cast<uint32_t>(ctx->r13.u64));
  uint32_t old_tls_ptr = memory::load_and_swap<uint32_t>(pcr_address);
  memory::store_and_swap<uint32_t>(pcr_address, 0);

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }

  // Restores TLS ptr.
  memory::store_and_swap<uint32_t>(pcr_address, old_tls_ptr);

  return ctx->r3.u64;
}

Irql Processor::RaiseIrql(Irql new_value) {
  return static_cast<Irql>(
      irql_.exchange(static_cast<uint32_t>(new_value), std::memory_order_acq_rel));
}

void Processor::LowerIrql(Irql old_value) {
  irql_.store(static_cast<uint32_t>(old_value), std::memory_order_release);
}

bool Processor::Save(stream::ByteStream* stream) {
  stream->Write(kProcessorSaveSignature);
  return true;
}

bool Processor::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kProcessorSaveSignature) {
    REXLOG_ERROR("Processor::Restore - Invalid magic value!");
    return false;
  }
  return true;
}

void Processor::OnThreadCreated(uint32_t thread_handle, ThreadState* thread_state, Thread* thread) {
  // TODO(tomc): Implement me, processor notification mgmt
  REXLOG_DEBUG("Thread created: handle={:08X}, id={}", thread_handle, thread_state->thread_id());
}

void Processor::OnThreadExit(uint32_t thread_id) {
  // TODO(tomc): Implement me, processor notification mgmt
  REXLOG_DEBUG("Thread exited: id={}", thread_id);
}

void Processor::OnThreadDestroyed(uint32_t thread_id) {
  // TODO(tomc): Implement me, processor notification mgmt
  REXLOG_DEBUG("Thread destroyed: id={}", thread_id);
}

// rexglue function table management
bool Processor::InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                                        uint32_t image_size) {
  if (function_table_initialized_) {
    REXLOG_WARN("Function table already initialized");
    return false;
  }

  // Initialize the guest memory function table (for PPC_LOOKUP_FUNC in recompiled code)
  if (!memory_->InitializeFunctionTable(code_base, code_size, image_base, image_size)) {
    REXLOG_ERROR("Failed to initialize guest memory function table");
    return false;
  }

  code_base_ = code_base;
  code_size_ = code_size;
  image_base_ = image_base;
  image_size_ = image_size;
  function_table_initialized_ = true;
  REXLOG_INFO("Processor function table initialized: code={:08X}-{:08X}, image={:08X}-{:08X}",
              code_base, code_base + code_size, image_base, image_base + image_size);
  return true;
}

void Processor::SetFunction(uint32_t guest_address, ::PPCFunc* func) {
  assert_true(function_table_initialized_);

  // Store in C++ map (for Processor::Execute/GetFunction)
  function_table_[guest_address] = func;

  // Also write to guest memory (for PPC_LOOKUP_FUNC in recompiled code)
  memory_->SetFunction(guest_address, func);
}

::PPCFunc* Processor::GetFunction(uint32_t guest_address) {
  auto it = function_table_.find(guest_address);
  if (it != function_table_.end()) {
    return it->second;
  }
  return nullptr;
}

}  // namespace rex::runtime
