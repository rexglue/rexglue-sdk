/**
 * @file        system/function_dispatcher.cpp
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

#include <rex/assert.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>

namespace rex::runtime {

FunctionDispatcher::FunctionDispatcher(rex::memory::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

FunctionDispatcher::~FunctionDispatcher() = default;

bool FunctionDispatcher::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");
  PROFILE_FUNCTION_DISPATCHED();

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

uint64_t FunctionDispatcher::Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
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

uint64_t FunctionDispatcher::ExecuteInterrupt(ThreadState* thread_state, uint32_t address,
                                              uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");
  PROFILE_INTERRUPT_DISPATCHED();

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

// rexglue function table management
bool FunctionDispatcher::InitializeFunctionTable(uint32_t code_base, uint32_t code_size,
                                                 uint32_t image_base, uint32_t image_size) {
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

  // Initialize thunk allocation region (immediately after code section)
  next_thunk_address_ = code_base + code_size;
  thunk_limit_ = next_thunk_address_ + 0x10000;
  REXLOG_INFO(
      "FunctionDispatcher function table initialized: code={:08X}-{:08X}, image={:08X}-{:08X}",
      code_base, code_base + code_size, image_base, image_base + image_size);
  return true;
}

void FunctionDispatcher::SetFunction(uint32_t guest_address, ::PPCFunc* func) {
  assert_true(function_table_initialized_);

  // Store in C++ map (for FunctionDispatcher::Execute/GetFunction)
  function_table_[guest_address] = func;

  // Also write to guest memory (for PPC_LOOKUP_FUNC in recompiled code)
  memory_->SetFunction(guest_address, func);
}

::PPCFunc* FunctionDispatcher::GetFunction(uint32_t guest_address) {
  auto it = function_table_.find(guest_address);
  if (it != function_table_.end()) {
    return it->second;
  }
  return nullptr;
}

uint32_t FunctionDispatcher::AllocateThunk(::PPCFunc* func) {
  if (next_thunk_address_ >= thunk_limit_) {
    REXLOG_ERROR("Thunk address space exhausted");
    return 0;
  }
  uint32_t addr = next_thunk_address_;
  next_thunk_address_ += 4;  // 4-byte aligned
  SetFunction(addr, func);
  return addr;
}

}  // namespace rex::runtime
