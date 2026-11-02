#include "kernel/ppc_hooks.h"

#include <mutex>
#include <optional>

#include <rex/assert.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/heap.h>
#include <rex/logging/log.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/kernel_hook.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

ppc::BoundThread ReadBoundThread() {
  const ThreadState* state = ThreadState::Get();
  if (!state) {
    return {nullptr, nullptr};
  }
  Memory* memory = state->GetMemory();
  return {state->GetContext(), memory ? memory->GetVirtualMembase() : nullptr};
}

FunctionDispatcher* GetBoundFunctionDispatcher() {
  ThreadState* state = ThreadState::Get();
  TitleRuntime* instance = state != nullptr ? state->GetRuntime() : nullptr;
  return instance != nullptr ? StateOf(*instance).GetFunctionDispatcher() : nullptr;
}

PPCFunc* ResolveBoundFunction(u32 guest_address) {
  FunctionDispatcher* dispatcher = GetBoundFunctionDispatcher();
  return dispatcher ? dispatcher->GetFunction(guest_address) : nullptr;
}

#ifndef NDEBUG
void CheckHookRegisterFile(const PPCContext& ctx, const char* name) {
  const auto* thread_state = ThreadState::Get();
  if (thread_state && &ctx != thread_state->GetContext()) {
    REXLOG_ERROR(cpu, "Hook {} runs on a register file that is not its thread's", name);
  }
  REX_ASSERT(!thread_state || &ctx == thread_state->GetContext(),
             "A hook runs on a register file that is not its thread's");
  REX_ASSERT(!thread_state || ctx.thread_state == thread_state,
             "A hook runs on a register file another thread state owns");
}
#endif

void CheckKernelDetourLocalWrites(const char* name, const PPCContext& at_entry,
                                  const PPCContext& at_exit, std::atomic_flag& warned) {
  const ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) {
    return;
  }
  ppc::detail::CheckDetourLocalWrites(name, StateOf(*state->GetRuntime()).GetCodegenFlags(),
                                      at_entry, at_exit, warned);
}

void CheckKernelDetourArguments(const char* name, PPCFunc* host, u8 declared_gpr,
                                u16 declared_fpr) {
  const ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) {
    return;
  }
  const std::optional<ppc::PPCCallUsage> usage = StateOf(*state->GetRuntime()).GetCallUsage(host);
  if (usage) {
    ppc::detail::CheckDetourArguments(name, *usage, declared_gpr, declared_fpr);
  }
}

std::mutex g_hooks_mutex;
u32 g_hooks_count = 0;

const ppc::KernelHooks kHooks = {
    ReadBoundThread,
    ResolveBoundFunction,
#ifndef NDEBUG
    CheckHookRegisterFile,
#else
    nullptr,
#endif
    CheckKernelDetourLocalWrites,
    CheckKernelDetourArguments,
};

}  // namespace

void RegisterPPCHooks() {
  std::lock_guard lock(g_hooks_mutex);
  if (g_hooks_count++ == 0) {
    ppc::SetKernelHooks(&kHooks);
  }
}

void UnregisterPPCHooks() {
  std::lock_guard lock(g_hooks_mutex);
  REX_ASSERT(g_hooks_count != 0, "The PPC hooks are released more often than registered");
  if (--g_hooks_count == 0) {
    ppc::SetKernelHooks(nullptr);
  }
}

}  // namespace rex::kernel
