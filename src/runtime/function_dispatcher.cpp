/**
 * @file        runtime/function_dispatcher.cpp
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

#include <csetjmp>
#include <cstdlib>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/assert.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xthread.h>

#include "runtime/dispatcher.h"

namespace rex::runtime {

namespace {

FunctionDispatcher* GetBoundFunctionDispatcher() {
  ThreadState* state = ThreadState::Get();
  Runtime* runtime = state != nullptr ? state->runtime() : nullptr;
  return runtime != nullptr ? runtime->function_dispatcher() : nullptr;
}

}  // namespace

static void InvalidFunctionTrap(PPCContext& ctx, uint8_t* /*base*/) {
  REX_FATAL(cpu, "Call to invalid or unregistered function at guest address 0x{:08X}",
            ctx.last_indirect_target);
}

PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  FunctionDispatcher* dispatcher = GetBoundFunctionDispatcher();
  if (!dispatcher) {
    return &InvalidFunctionTrap;
  }

  if (PPCFunc* func = dispatcher->GetFunction(guest_address)) {
    return func;
  }

  return &InvalidFunctionTrap;
}

}  // namespace rex::runtime

namespace rex::ppc {

void Trap(PPCContext& ctx, uint8_t* base, uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26: {
      const u32 address = ctx.r3.u32;
      const u32 offset = address >= 0xE0000000u ? 0x1000u : 0u;
      const std::string_view text(reinterpret_cast<const char*>(base + address + offset),
                                  ctx.r4.u16);
      REXLOG_DEBUG(cpu, "(service trap) {}", text);
      break;
    }
    case 0:
    case 22:
      REXLOG_WARN(cpu, "tw/td trap hit (type {})", trap_type);
      break;
    case 25:
      break;
    default:
      REXLOG_WARN(cpu, "Unknown trap type {}", trap_type);
      break;
  }
}

namespace {
thread_local std::unordered_map<uint32_t, std::jmp_buf> t_jump_buffers;
}  // namespace

std::jmp_buf& JumpBuffer(uint32_t address) {
  return t_jump_buffers[address];
}

void LongJump(uint32_t address, int value) {
  const auto it = t_jump_buffers.find(address);
  if (it != t_jump_buffers.end()) {
    std::longjmp(it->second, value);
  }
  std::abort();
}

void Unimplemented(const char* opcode) {
  throw std::runtime_error(std::string("Unimplemented PPC instruction: ") + opcode);
}

}  // namespace rex::ppc

namespace rex::runtime {

FunctionDispatcher::FunctionDispatcher(rex::runtime::Memory* memory) : memory_(memory) {}

FunctionDispatcher::~FunctionDispatcher() {
  std::lock_guard lock(recompiled_code_mutex_);
  for (const auto& [id, code] : recompiled_code_) {
    rex::platform::RemoveRecompiledCode(code);
  }
}

bool FunctionDispatcher::Run(ThreadState* thread_state, uint32_t address,
                             std::span<const uint64_t> args) {
  REX_PROFILE_SCOPE("cpu");
  REX_PERF_INC(kFunctionsDispatched);

  PPCFunc* fn = GetFunction(address);
  if (!fn) {
    REX_FAIL(cpu, false, "Execute({:08X}): function not in function table", address);
  }

  auto* ctx = thread_state->context();
  auto* previous_thread_state = ThreadState::Get();
  ThreadState::Bind(thread_state);

  const u32 stack_bytes = args.size() > 8 ? static_cast<u32>(args.size() - 8) * 8 : 0;
  const uint64_t previous_lr = ctx->lr;
  const u32 caller_r1 = ppc::OpenCallFrame(*ctx, memory_->virtual_membase(), stack_bytes);

  PPCRegister* registers[] = {&ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6,
                              &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10};
  for (const auto [index, value] : std::views::enumerate(args)) {
    if (index < 8) {
      registers[index]->u64 = value;
    } else {
      *memory_->TranslateVirtual<be_u32*>(ctx->r1.u32 + 0x54 + static_cast<u32>(index - 8) * 8) =
          static_cast<u32>(value);
    }
  }

  fn(*ctx, memory_->virtual_membase());

  ctx->lr = previous_lr;
  ctx->r1.u64 = caller_r1;
  ThreadState::Bind(previous_thread_state);

  return true;
}

uint64_t FunctionDispatcher::Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
                                     size_t arg_count) {
  auto* ctx = thread_state->context();
  const PPCContext saved = *ctx;
  const uint64_t result =
      Run(thread_state, address, std::span(args, arg_count)) ? ctx->r3.u64 : 0xDEADBABE;
  const PPCFPSCRRegister fpscr = ctx->fpscr;
  *ctx = saved;
  ctx->fpscr = fpscr;
  return result;
}

uint64_t FunctionDispatcher::ExecuteTrap(ThreadState* thread_state, uint32_t address,
                                         uint64_t args[], size_t arg_count) {
  auto* ctx = thread_state->context();
  const PPCContext saved = *ctx;
  const u32 saved_csr = ctx->fpscr.getcsr();

  const uint64_t result = Execute(thread_state, address, args, arg_count);

  *ctx = saved;
  ctx->fpscr.csr = saved_csr;
  ctx->fpscr.setcsr(saved_csr);
  return result;
}

uint64_t FunctionDispatcher::RunInterrupt(ThreadState* thread_state, uint32_t address,
                                          uint64_t args[], size_t arg_count) {
  std::lock_guard interrupt_lock(interrupt_mutex_);

  auto* ctx = thread_state->context();
  REX_ASSERT(arg_count <= 5);
  const PPCContext saved = *ctx;
  const u32 saved_csr = ctx->fpscr.getcsr();

  auto* pcr = memory_->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  const u32 old_tls_ptr = pcr->tls_ptr;
  const u8 old_irql = pcr->current_irql;
  pcr->tls_ptr = 0;
  pcr->current_irql = IRQL_DPC;

  const bool executed = Run(thread_state, address, std::span(args, arg_count));
  const uint64_t result = executed ? ctx->r3.u64 : 0xDEADBABE;

  pcr->current_irql = old_irql;
  pcr->tls_ptr = old_tls_ptr;
  *ctx = saved;
  ctx->fpscr.csr = saved_csr;
  ctx->fpscr.setcsr(saved_csr);
  return result;
}

uint64_t FunctionDispatcher::ExecuteInterrupt(ThreadState* thread_state, uint32_t address,
                                              uint64_t args[], size_t arg_count) {
  REX_PROFILE_SCOPE("cpu");
  REX_PERF_INC(kInterruptDispatches);

  EnterInterrupt();
  const uint64_t result = RunInterrupt(thread_state, address, args, arg_count);
  if (LeaveInterrupt()) {
    if (Runtime* runtime = thread_state->runtime()) {
      ThreadState* previous = ThreadState::Get();
      ThreadState::Bind(thread_state);
      runtime->dispatcher().DrainDPCs();
      ThreadState::Bind(previous);
    }
  }
  return result;
}

// rexglue function table management

bool FunctionDispatcher::InitializeFunctionTable(uint32_t code_base, uint32_t code_size,
                                                 uint32_t image_base, uint32_t image_size,
                                                 bool is_entrypoint) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);

  if (is_entrypoint && entrypoint_code_base_ != 0) {
    REX_FAIL(cpu, false, "InitializeFunctionTable: entrypoint already registered at {:08X}",
             entrypoint_code_base_);
  }

  uint32_t new_table_end = image_base + image_size + (code_size + kThunkReserveSize) * 2;
  uint32_t new_code_end = code_base + code_size + kThunkReserveSize;
  for (const auto& existing : module_tables_) {
    uint32_t existing_table_end =
        existing.image_base + existing.image_size + (existing.code_size + kThunkReserveSize) * 2;
    uint32_t existing_code_end = existing.code_base + existing.code_size + kThunkReserveSize;
    if (image_base < existing_table_end && new_table_end > existing.image_base) {
      REX_FAIL(cpu, false, "Module image range [{:08X}, {:08X}) overlaps existing [{:08X}, {:08X})",
               image_base, new_table_end, existing.image_base, existing_table_end);
    }
    if (code_base < existing_code_end && new_code_end > existing.code_base) {
      REX_FAIL(cpu, false, "Module code range [{:08X}, {:08X}) overlaps existing [{:08X}, {:08X})",
               code_base, new_code_end, existing.code_base, existing_code_end);
    }
  }

  if (!memory_->InitializeFunctionTable(code_base, code_size, image_base, image_size)) {
    REX_FAIL(cpu, false, "Failed to initialize guest memory function table");
  }

  module_tables_.push_back({
      .code_base = code_base,
      .code_size = code_size,
      .image_base = image_base,
      .image_size = image_size,
      .next_thunk_address = code_base + code_size,
      .thunk_limit = code_base + code_size + kThunkReserveSize,
  });

  if (is_entrypoint) {
    entrypoint_code_base_ = code_base;
  }

  REXLOG_DEBUG(cpu,
               "Function table initialized for module: code={:08X}-{:08X}, image={:08X}-{:08X}",
               code_base, code_base + code_size, image_base, image_base + image_size);
  return true;
}

FunctionDispatcher::ModuleTableInfo* FunctionDispatcher::FindModuleByAddress(
    uint32_t guest_address) {
  for (auto& mod : module_tables_) {
    if (guest_address >= mod.code_base && guest_address < mod.thunk_limit) {
      return &mod;
    }
  }
  return nullptr;
}

uint32_t FunctionDispatcher::FindCallerModuleBase(uint32_t guest_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  if (auto* mod = FindModuleByAddress(guest_address)) {
    return mod->code_base;
  }
  return 0;
}

bool FunctionDispatcher::SetFunction(uint32_t guest_address, ::PPCFunc* func) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  REX_ASSERT(!module_tables_.empty());

  if (!FindModuleByAddress(guest_address)) {
    REX_FAIL(cpu, false, "SetFunction: address {:08X} outside all registered module ranges",
             guest_address);
  }

  function_table_[guest_address] = func;

  if (!memory_->SetFunction(guest_address, func)) {
    REXLOG_ERROR(cpu, "SetFunction: dispatcher / Memory module-table state out of sync at {:08X}",
                 guest_address);
    function_table_.erase(guest_address);
    return false;
  }

  if (recording_) {
    recording_addresses_.push_back(guest_address);
  }
  return true;
}

::PPCFunc* FunctionDispatcher::GetFunction(uint32_t guest_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto it = function_table_.find(guest_address);
  if (it != function_table_.end()) {
    return it->second;
  }
  return nullptr;
}

uint32_t FunctionDispatcher::AllocateThunk(::PPCFunc* func, uint32_t caller_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto* mod = FindModuleByAddress(caller_address);
  if (!mod) {
    if (caller_address != 0) {
      REX_FAIL(cpu, 0, "AllocateThunk: caller_address {:08X} not in any registered module",
               caller_address);
    }
    if (entrypoint_code_base_ == 0) {
      REX_FAIL(cpu, 0, "AllocateThunk: caller_address=0 but no entrypoint registered");
    }
    mod = FindModuleByAddress(entrypoint_code_base_);
    if (!mod) {
      REX_FAIL(cpu, 0, "AllocateThunk: entrypoint code_base {:08X} not in module_tables_",
               entrypoint_code_base_);
    }
  }

  if (mod->next_thunk_address >= mod->thunk_limit) {
    REX_FAIL(cpu, 0, "Thunk address space exhausted for module at {:08X}", mod->code_base);
  }
  uint32_t addr = mod->next_thunk_address;
  mod->next_thunk_address += 4;
  if (!SetFunction(addr, func)) {
    mod->next_thunk_address -= 4;
    return 0;
  }
  return addr;
}

void FunctionDispatcher::RegisterModule(const std::string& module_id, uint32_t code_base,
                                        RegisterFn register_func,
                                        std::span<const u8> recompiled_code) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  if (recording_) {
    REX_FATAL(cpu, "RegisterModule called while already recording (re-entrancy)");
    return;
  }

  if (module_addresses_.find(module_id) != module_addresses_.end()) {
    REXLOG_WARN(cpu, "RegisterModule: '{}' is already registered; cleaning up prior batch",
                module_id);
    UnregisterModule(module_id);
  }

  REXLOG_DEBUG(cpu, "Registering module: {} (code_base={:08X})", module_id, code_base);

  recording_addresses_.clear();
  recording_ = true;

  struct RecordingGuard {
    FunctionDispatcher* self;
    ~RecordingGuard() {
      self->recording_ = false;
      self->recording_addresses_.clear();
    }
  } guard{this};

  register_func(this);

  ModuleRegistration reg;
  reg.code_base = code_base;
  reg.addresses = std::move(recording_addresses_);
  if (!recompiled_code.empty() && rex::platform::AddRecompiledCode(recompiled_code)) {
    std::lock_guard host_lock(recompiled_code_mutex_);
    recompiled_code_.emplace_back(module_id, recompiled_code);
  }

  size_t count = reg.addresses.size();
  module_addresses_[module_id] = std::move(reg);

  REXLOG_INFO(cpu, "Module '{}' registered {} functions", module_id, count);
}

void FunctionDispatcher::AddRecompiledCode(std::span<const u8> code) {
  if (code.empty()) {
    return;
  }
  if (!rex::platform::AddRecompiledCode(code)) {
    REXLOG_ERROR(cpu, "AddRecompiledCode: no slot left for {} bytes of recompiled code",
                 code.size());
    return;
  }
  std::lock_guard lock(recompiled_code_mutex_);
  recompiled_code_.emplace_back(std::string(), code);
}

std::optional<std::pair<uint32_t, uint32_t>> FunctionDispatcher::UnregisterModule(
    const std::string& module_id) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto it = module_addresses_.find(module_id);
  if (it == module_addresses_.end()) {
    REXLOG_WARN(cpu, "UnregisterModule: module '{}' not found", module_id);
    return std::nullopt;
  }
  {
    std::lock_guard host_lock(recompiled_code_mutex_);
    std::erase_if(recompiled_code_, [&module_id](const auto& entry) {
      if (entry.first != module_id) {
        return false;
      }
      rex::platform::RemoveRecompiledCode(entry.second);
      return true;
    });
  }

  REXLOG_DEBUG(cpu, "Unregistering module: {} ({} functions)", module_id,
               it->second.addresses.size());

  auto table_it = std::find_if(module_tables_.begin(), module_tables_.end(),
                               [code_base = it->second.code_base](const ModuleTableInfo& mti) {
                                 return mti.code_base == code_base;
                               });

  for (uint32_t addr : it->second.addresses) {
    function_table_.erase(addr);
    memory_->SetFunction(addr, nullptr);
  }

  std::optional<std::pair<uint32_t, uint32_t>> cleared_range;
  if (table_it != module_tables_.end()) {
    uint32_t pool_start = table_it->code_base + table_it->code_size;
    uint32_t pool_end = table_it->next_thunk_address;
    for (uint32_t addr = pool_start; addr < pool_end; addr += 4) {
      function_table_.erase(addr);
      memory_->SetFunction(addr, nullptr);
    }
    cleared_range = std::make_pair(pool_start, pool_end);

    if (table_it->code_base == entrypoint_code_base_) {
      entrypoint_code_base_ = 0;
    }
    memory_->DestroyFunctionTable(table_it->code_base);
    module_tables_.erase(table_it);
  }

  module_addresses_.erase(it);

  return cleared_range;
}

}  // namespace rex::runtime
