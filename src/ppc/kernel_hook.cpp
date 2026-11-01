#include <atomic>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/kernel_hook.h>
#include <rex/ppc/recompiled_code.h>

namespace rex::ppc {

namespace {

std::atomic<const KernelHooks*> g_hooks{nullptr};

const KernelHooks* GetHooks() {
  return g_hooks.load(std::memory_order_acquire);
}

void InvalidFunctionTrap(PPCContext& ctx, u8* /*base*/) {
  REX_FATAL(cpu, "Call to invalid or unregistered function at guest address 0x{:08X}",
            ctx.last_indirect_target);
}

}  // namespace

void SetKernelHooks(const KernelHooks* hooks) {
  g_hooks.store(hooks, std::memory_order_release);
}

BoundThread GetBoundThread() {
  const KernelHooks* hooks = GetHooks();
  if (hooks == nullptr || hooks->get_bound_thread == nullptr) {
    return {nullptr, nullptr};
  }
  return hooks->get_bound_thread();
}

PPCFunc* ResolveFunction(u32 guest_address) {
  const KernelHooks* hooks = GetHooks();
  if (hooks != nullptr && hooks->resolve_function != nullptr) {
    if (PPCFunc* func = hooks->resolve_function(guest_address)) {
      return func;
    }
  }
  return &InvalidFunctionTrap;
}

#ifndef NDEBUG
void detail::CheckHookContext(const PPCContext& ctx, const char* name) {
  const KernelHooks* hooks = GetHooks();
  if (hooks != nullptr && hooks->check_hook_context != nullptr) {
    hooks->check_hook_context(ctx, name);
  }
}
#endif

void detail::CheckBoundDetourLocalWrites(const char* name, const PPCContext& at_entry,
                                         const PPCContext& at_exit, std::atomic_flag& warned) {
  const KernelHooks* hooks = GetHooks();
  if (hooks != nullptr && hooks->check_bound_detour_local_writes != nullptr) {
    hooks->check_bound_detour_local_writes(name, at_entry, at_exit, warned);
  }
}

void detail::CheckBoundDetourArguments(const char* name, PPCFunc* host, u8 declared_gpr,
                                       u16 declared_fpr) {
  const KernelHooks* hooks = GetHooks();
  if (hooks != nullptr && hooks->check_bound_detour_arguments != nullptr) {
    hooks->check_bound_detour_arguments(name, host, declared_gpr, declared_fpr);
  }
}

}  // namespace rex::ppc
