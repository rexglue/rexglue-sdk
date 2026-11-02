#include <cstdlib>
#include <string>
#include <string_view>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/platform/perf.h>
#include <rex/kernel/types/thread.h>
#include <rex/logging/log.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/kernel_thread.h>

#include "kernel/dispatcher.h"
#include "kernel/title_state.h"
#include "kernel/title_state.h"

namespace rex::kernel {

FunctionDispatcher::FunctionDispatcher(rex::kernel::Memory* memory) : memory_(memory) {}

FunctionDispatcher::~FunctionDispatcher() {
  std::lock_guard lock(recompiled_code_mutex_);
  for (const auto& [id, code] : recompiled_code_) {
    rex::ppc::RemoveRecompiledCode(code);
  }
}

bool FunctionDispatcher::Run(ThreadState* thread_state, u32 address, std::span<const u64> args) {
  REX_PROFILE_SCOPE("cpu");
  REX_PERF_INC("functions_dispatched");

  PPCFunc* fn = GetFunction(address);
  if (!fn) {
    REX_FAIL(cpu, false, "Run({:08X}): function not in function table", address);
  }

  auto* ctx = thread_state->GetContext();
  auto* previous_thread_state = ThreadState::Get();
  ThreadState::Bind(thread_state);

  const u32 stack_bytes = args.size() > 8 ? static_cast<u32>(args.size() - 8) * 8 : 0;
  const u64 previous_lr = ctx->lr;
  const u32 caller_r1 = ppc::OpenCallFrame(*ctx, memory_->GetVirtualMembase(), stack_bytes);

  PPCRegister* registers[] = {&ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6,
                              &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10};
  for (size_t index = 0; index < args.size(); ++index) {
    const u64 value = args[index];
    if (index < 8) {
      registers[index]->u64 = value;
    } else {
      *memory_->TranslateVirtual<be_u64*>(ctx->r1.u32 + 0x50 + static_cast<u32>(index - 8) * 8) =
          value;
    }
  }

  fn(*ctx, memory_->GetVirtualMembase());

  ctx->lr = previous_lr;
  ctx->r1.u64 = caller_r1;
  ThreadState::Bind(previous_thread_state);

  return true;
}

u64 FunctionDispatcher::Execute(ThreadState* thread_state, u32 address, u64 args[],
                                size_t arg_count) {
  auto* ctx = thread_state->GetContext();
  const PPCContext saved = *ctx;
  const u64 result =
      Run(thread_state, address, std::span(args, arg_count)) ? ctx->r3.u64 : 0xDEADBABE;
  const PPCFPSCRRegister fpscr = ctx->fpscr;
  *ctx = saved;
  ctx->fpscr = fpscr;
  return result;
}

u64 FunctionDispatcher::ExecuteTrap(ThreadState* thread_state, u32 address, u64 args[],
                                    size_t arg_count) {
  auto* ctx = thread_state->GetContext();
  const PPCContext saved = *ctx;
  const u32 saved_csr = ctx->fpscr.GetCSR();

  const u64 result = Execute(thread_state, address, args, arg_count);

  *ctx = saved;
  ctx->fpscr.csr = saved_csr;
  ctx->fpscr.SetCSR(saved_csr);
  return result;
}

u64 FunctionDispatcher::RunInterrupt(ThreadState* thread_state, u32 address, u64 args[],
                                     size_t arg_count) {
  std::lock_guard interrupt_lock(interrupt_mutex_);

  auto* ctx = thread_state->GetContext();
  REX_ASSERT(arg_count <= 5);
  const PPCContext saved = *ctx;
  const u32 saved_csr = ctx->fpscr.GetCSR();

  auto* pcr = memory_->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  const u32 old_tls_ptr = pcr->tls_ptr;
  const u8 old_irql = pcr->current_irql;
  pcr->tls_ptr = 0;
  pcr->current_irql = kIRQLDPC;

  const bool executed = Run(thread_state, address, std::span(args, arg_count));
  const u64 result = executed ? ctx->r3.u64 : 0xDEADBABE;

  pcr->current_irql = old_irql;
  pcr->tls_ptr = old_tls_ptr;
  *ctx = saved;
  ctx->fpscr.csr = saved_csr;
  ctx->fpscr.SetCSR(saved_csr);
  return result;
}

u64 FunctionDispatcher::ExecuteInterrupt(ThreadState* thread_state, u32 address, u64 args[],
                                         size_t arg_count) {
  REX_PROFILE_SCOPE("cpu");
  REX_PERF_INC("interrupt_dispatches");

  EnterInterrupt();
  const u64 result = RunInterrupt(thread_state, address, args, arg_count);
  if (LeaveInterrupt()) {
    if (TitleRuntime* runtime = thread_state->GetRuntime()) {
      ThreadState* previous = ThreadState::Get();
      ThreadState::Bind(thread_state);
      StateOf(*runtime).GetDPCQueue().Drain();
      ThreadState::Bind(previous);
    }
  }
  return result;
}

void FunctionDispatcher::SetGraphicsInterrupt(u32 callback, u32 user_data) {
  graphics_interrupt_.store(callback | (u64{user_data} << 32), std::memory_order_release);
}

void FunctionDispatcher::RaiseGraphicsInterrupt(u32 source, u32 cpu) {
  const u64 interrupt = graphics_interrupt_.load(std::memory_order_acquire);
  const u32 callback = static_cast<u32>(interrupt);
  if (!callback) {
    return;
  }
  KernelThread* thread = KernelThread::GetCallingThread();
  REX_ASSERT(thread != nullptr);
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCPU(cpu);
  u64 args[] = {source, interrupt >> 32};
  ExecuteInterrupt(thread->GetThreadState(), callback, args, rex::CountOf(args));
}

bool FunctionDispatcher::InitializeFunctionTable(u32 code_base, u32 code_size, u32 image_base,
                                                 u32 image_size, bool is_entrypoint) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);

  if (is_entrypoint && entrypoint_code_base_ != 0) {
    REX_FAIL(cpu, false, "InitializeFunctionTable: entrypoint already registered at {:08X}",
             entrypoint_code_base_);
  }

  u32 new_table_end = image_base + image_size + (code_size + kThunkReserveSize) * 2;
  u32 new_code_end = code_base + code_size + kThunkReserveSize;
  for (const auto& existing : module_tables_) {
    u32 existing_table_end =
        existing.image_base + existing.image_size + (existing.code_size + kThunkReserveSize) * 2;
    u32 existing_code_end = existing.code_base + existing.code_size + kThunkReserveSize;
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

FunctionDispatcher::ModuleTable* FunctionDispatcher::FindModuleByAddress(u32 guest_address) {
  for (auto& mod : module_tables_) {
    if (guest_address >= mod.code_base && guest_address < mod.thunk_limit) {
      return &mod;
    }
  }
  return nullptr;
}

u32 FunctionDispatcher::FindCallerModuleBase(u32 guest_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  if (auto* mod = FindModuleByAddress(guest_address)) {
    return mod->code_base;
  }
  return 0;
}

bool FunctionDispatcher::SetFunction(u32 guest_address, ::PPCFunc* func) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  REX_ASSERT(!module_tables_.empty());

  if (!FindModuleByAddress(guest_address)) {
    REX_FAIL(cpu, false, "SetFunction: address {:08X} outside all registered module ranges",
             guest_address);
  }

  function_table_[guest_address] = func;

  if (!memory_->SetFunction(guest_address, func)) {
    REXLOG_ERROR(cpu, "SetFunction: dispatcher / Memory module table state out of sync at {:08X}",
                 guest_address);
    function_table_.erase(guest_address);
    return false;
  }

  if (registering_) {
    registered_addresses_.push_back(guest_address);
  }
  return true;
}

::PPCFunc* FunctionDispatcher::GetFunction(u32 guest_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto it = function_table_.find(guest_address);
  if (it != function_table_.end()) {
    return it->second;
  }
  return nullptr;
}

u32 FunctionDispatcher::AllocateThunk(::PPCFunc* func, u32 caller_address) {
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
  u32 addr = mod->next_thunk_address;
  mod->next_thunk_address += 4;
  if (!SetFunction(addr, func)) {
    mod->next_thunk_address -= 4;
    return 0;
  }
  return addr;
}

void FunctionDispatcher::RegisterModule(const std::string& module_id, u32 code_base,
                                        RegisterFn register_func,
                                        std::span<const u8> recompiled_code) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  if (registering_) {
    REX_FATAL(cpu, "RegisterModule called while already registering (reentrancy)");
    return;
  }

  if (module_addresses_.find(module_id) != module_addresses_.end()) {
    REXLOG_WARN(cpu, "RegisterModule: '{}' is already registered, cleaning up prior batch",
                module_id);
    UnregisterModule(module_id);
  }

  REXLOG_DEBUG(cpu, "Registering module: {} (code_base={:08X})", module_id, code_base);

  registered_addresses_.clear();
  registering_ = true;

  struct RegistrationGuard {
    FunctionDispatcher* self;
    ~RegistrationGuard() {
      self->registering_ = false;
      self->registered_addresses_.clear();
    }
  } guard{this};

  register_func(this);

  ModuleRegistration reg;
  reg.code_base = code_base;
  reg.addresses = std::move(registered_addresses_);
  if (!recompiled_code.empty() && rex::ppc::AddRecompiledCode(recompiled_code)) {
    std::lock_guard recompiled_code_lock(recompiled_code_mutex_);
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
  if (!rex::ppc::AddRecompiledCode(code)) {
    REXLOG_ERROR(cpu, "AddRecompiledCode: no slot left for {} bytes of recompiled code",
                 code.size());
    return;
  }
  std::lock_guard lock(recompiled_code_mutex_);
  recompiled_code_.emplace_back(std::string(), code);
}

std::optional<std::pair<u32, u32>> FunctionDispatcher::UnregisterModule(
    const std::string& module_id) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto it = module_addresses_.find(module_id);
  if (it == module_addresses_.end()) {
    REXLOG_WARN(cpu, "UnregisterModule: module '{}' not found", module_id);
    return std::nullopt;
  }
  {
    std::lock_guard recompiled_code_lock(recompiled_code_mutex_);
    std::erase_if(recompiled_code_, [&module_id](const auto& entry) {
      if (entry.first != module_id) {
        return false;
      }
      rex::ppc::RemoveRecompiledCode(entry.second);
      return true;
    });
  }

  REXLOG_DEBUG(cpu, "Unregistering module: {} ({} functions)", module_id,
               it->second.addresses.size());

  auto table_it = std::find_if(module_tables_.begin(), module_tables_.end(),
                               [code_base = it->second.code_base](const ModuleTable& mti) {
                                 return mti.code_base == code_base;
                               });

  for (u32 addr : it->second.addresses) {
    function_table_.erase(addr);
    memory_->SetFunction(addr, nullptr);
  }

  std::optional<std::pair<u32, u32>> cleared_range;
  if (table_it != module_tables_.end()) {
    u32 pool_start = table_it->code_base + table_it->code_size;
    u32 pool_end = table_it->next_thunk_address;
    for (u32 addr = pool_start; addr < pool_end; addr += 4) {
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

}  // namespace rex::kernel
