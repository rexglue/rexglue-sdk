/**
 * @file        kernel/shim/threading.cpp
 * @brief       Shim fiber routines
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <cstring>
#include <memory>
#include <tuple>

#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/kernel/shim/define.h>
#include <rex/types.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include <rex/runtime/process.h>
#include <rex/platform/fiber.h>
#include "runtime/guest_stack.h"

namespace rex::kernel::shim {
using namespace rex::runtime;

namespace {

std::atomic<u32> unique_fiber_count;

/// get KTHREAD and PCR pointers from the current thread's context.
struct GuestThreadPtrs {
  X_KTHREAD* kthread;
  X_KPCR* pcr;
  PPCContext* ctx;
  runtime::Memory* mem;
};

GuestThreadPtrs GetGuestThreadPtrs(ThreadRecord* thread) {
  auto* ctx = thread->thread_state()->context();
  auto* mem = thread->runtime().memory();
  auto* pcr = mem->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  auto* kthread =
      mem->TranslateVirtual<X_KTHREAD*>(static_cast<u32>(pcr->prcb_data.current_thread));
  return {kthread, pcr, ctx, mem};
}

/// update KTHREAD/PCR/ctx stack pointers
void UpdateGuestStackPointers(X_KTHREAD* kthread, X_KPCR* pcr, PPCContext* ctx, u32 sp,
                              u32 stack_alloc_base, u32 stack_base, u32 stack_limit) {
  kthread->stack_alloc_base = stack_alloc_base;
  kthread->stack_base = stack_base;
  kthread->stack_limit = stack_limit;
  pcr->stack_base_ptr = stack_base;
  pcr->stack_end_ptr = stack_limit;
  ctx->r1.u64 = sp;
}

//-----------------------------------------------------------------------------
// FiberEntryPoint -- host fiber entry for CreateFiber fibers
//-----------------------------------------------------------------------------

struct FiberEntryArgs {
  PPCFunc* start_fn;
  u32 guest_fiber_addr;
  Runtime* runtime;
};

static void FiberEntryPoint(void* raw_arg) {
  auto args = std::unique_ptr<FiberEntryArgs>(static_cast<FiberEntryArgs*>(raw_arg));
  auto* thread = ThreadRecord::GetCurrentThread();
  auto* mem = args->runtime->memory();
  PPCContext& ctx = *thread->thread_state()->context();
  ctx.fpscr.setcsr(ctx.fpscr.csr);

  // Read fiber_data (lpParameter) from guest context buffer
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(args->guest_fiber_addr);
  ctx.r3.u64 = static_cast<u32>(fiber->fiber_data);

  // Call the fiber function
  args->start_fn(ctx, mem->virtual_membase());

  // Fiber returned (shouldn't per XDK) -- safe fallback
  if (thread->main_fiber()) {
    REX_PROFILE_FIBER_LEAVE();
    rex::platform::Fiber::SwitchTo(thread->main_fiber());
  }
  REXLOG_WARN(kernel, "FiberEntryPoint: fiber function returned with no main fiber - terminating");
  std::terminate();
}

/// Helper for ConvertFiberToThread logic, shared by ConvertFiberToThread_shim
/// and DeleteFiber_shim (self-delete case).
u32 ConvertFiberToThread_impl(ThreadRecord* thread) {
  Runtime& runtime = thread->runtime();
  auto [kthread, pcr, ctx, mem] = GetGuestThreadPtrs(thread);

  u32 fiber_addr = kthread->fiber_ptr;
  if (!fiber_addr) {
    kthread->last_error = 0x501;  // ERROR_ALREADY_THREAD
    return 0;
  }

  auto info = runtime.thread_registry().LookupFiber(fiber_addr);
  if (info) {
    info->host_fiber->Destroy();
    runtime.thread_registry().UnregisterFiber(fiber_addr);
  }

  kthread->fiber_ptr = 0u;
  // Must null main_fiber_ AFTER Destroy() above to avoid double-free
  // in ~ThreadRecord, which also calls main_fiber_->Destroy() if non-null.
  thread->set_main_fiber(nullptr);
  mem->SystemHeapFree(fiber_addr);

  return 1;  // TRUE
}

}  // namespace

//=============================================================================
// XAPI Fiber Function Implementations
//=============================================================================

REX_SHIM_FN(ConvertThreadToFiber, u32, mapped_void lpParameter) {
  auto* thread = ThreadRecord::GetCurrentThread();
  Runtime& runtime = thread->runtime();
  auto [kthread, pcr, ctx, mem] = GetGuestThreadPtrs(thread);

  if (kthread->fiber_ptr) {
    kthread->last_error = 0x500;  // ERROR_ALREADY_FIBER
    return 0;
  }

  // Allocate guest fiber context buffer
  u32 buf_addr = mem->SystemHeapAlloc(sizeof(X_FIBER_CONTEXT));
  if (!buf_addr) {
    kthread->last_error = 8;  // ERROR_NOT_ENOUGH_MEMORY
    return 0;
  }
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(buf_addr);
  std::memset(fiber, 0, sizeof(X_FIBER_CONTEXT));

  // Populate from current KTHREAD stack state
  fiber->fiber_data = lpParameter.guest_address();
  fiber->stack_alloc_base = kthread->stack_alloc_base;
  fiber->stack_base = kthread->stack_base;
  fiber->stack_limit = kthread->stack_limit;
  fiber->sp_save = ctx->r1.u64;

  kthread->fiber_ptr = buf_addr;

  // Reuse existing host fiber from ThreadRecord::Execute() if available;
  // otherwise create one (should not happen in normal flow).
  auto* host_fiber = thread->main_fiber();
  if (!host_fiber) {
    host_fiber = rex::platform::Fiber::ConvertCurrentThread();
    thread->set_main_fiber(host_fiber);
  }

  runtime.thread_registry().RegisterFiber(
      buf_addr, FiberInfo{host_fiber, unique_fiber_count++, buf_addr, 0, true});

  REXLOG_DEBUG(kernel, "ConvertThreadToFiber: fiber={:#010x} param={:#010x}", buf_addr,
               lpParameter.guest_address());
  return buf_addr;
}

REX_SHIM_FN(ConvertFiberToThread, u32) {
  auto* thread = ThreadRecord::GetCurrentThread();
  auto result = ConvertFiberToThread_impl(thread);
  if (result) {
    REXLOG_DEBUG(kernel, "ConvertFiberToThread: success");
  }
  return result;
}

REX_SHIM_FN(CreateFiber, u32, u32 dwStackSize, u32 lpStartAddress, mapped_void lpParameter) {
  auto* thread = ThreadRecord::GetCurrentThread();
  Runtime& runtime = thread->runtime();
  auto* mem = runtime.memory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);

  const auto* process =
      mem->TranslateVirtual<const X_KPROCESS*>(static_cast<u32>(kthread->process));
  const u32 guest_stack_size =
      ResolveGuestStackSize(dwStackSize, process->kernel_stack_size, kMinimumFiberStackSize);
  Result<GuestStack> stack = AllocateGuestStack(*mem, guest_stack_size, 0, true);
  if (!stack) {
    kthread->last_error = 8;  // ERROR_NOT_ENOUGH_MEMORY
    return 0;
  }
  const u32 stack_top = stack->base;
  const u32 stack_bottom = stack->limit;
  const u32 initial_sp = stack_top - 0x50;
  std::memset(mem->TranslateVirtual(initial_sp), 0, 0x50);

  PPCFunc* start_fn = runtime.function_dispatcher()->GetFunction(lpStartAddress);

  u32 buf_addr = mem->SystemHeapAlloc(sizeof(X_FIBER_CONTEXT));
  if (!buf_addr) {
    FreeGuestStack(*mem, stack->alloc_base);
    kthread->last_error = 8;
    return 0;
  }
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(buf_addr);
  std::memset(fiber, 0, sizeof(X_FIBER_CONTEXT));

  fiber->fiber_data = lpParameter.guest_address();
  fiber->stack_alloc_base = stack_top;
  fiber->stack_base = stack_top;
  fiber->stack_limit = stack_bottom;
  fiber->sp_save = initial_sp;

  size_t host_stack =
      std::max(static_cast<size_t>(guest_stack_size), static_cast<size_t>(256u * 1024u));
  auto args_owner = std::make_unique<FiberEntryArgs>(FiberEntryArgs{
      start_fn,
      buf_addr,
      &runtime,
  });
  auto* host_fiber = rex::platform::Fiber::Create(host_stack, FiberEntryPoint, args_owner.get());
  if (host_fiber) {
    std::ignore = args_owner.release();
  }

  runtime.thread_registry().RegisterFiber(
      buf_addr, FiberInfo{host_fiber, unique_fiber_count++, buf_addr, stack->alloc_base, false});

  REXLOG_DEBUG(kernel, "CreateFiber: fiber={:#010x} start={:#010x} stack={:#x} param={:#010x}",
               buf_addr, lpStartAddress, guest_stack_size, lpParameter.guest_address());
  return buf_addr;
}

REX_SHIM_FN(DeleteFiber, void, mapped_void lpFiber) {
  auto* thread = ThreadRecord::GetCurrentThread();
  Runtime& runtime = thread->runtime();
  auto* mem = runtime.memory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  u32 fiber_addr = lpFiber.guest_address();

  // Self-delete: ConvertFiberToThread + ExitThread
  if (static_cast<u32>(kthread->fiber_ptr) == fiber_addr) {
    REXLOG_DEBUG(kernel, "DeleteFiber: self-delete fiber={:#010x}", fiber_addr);
    REX_PROFILE_FIBER_LEAVE();
    ConvertFiberToThread_impl(thread);
    thread->Exit(1);  // does not return
    return;
  }

  REXLOG_DEBUG(kernel, "DeleteFiber: fiber={:#010x}", fiber_addr);

  auto info = runtime.thread_registry().LookupFiber(fiber_addr);
  if (info) {
    if (!info->is_thread_fiber) {
      FreeGuestStack(*mem, info->guest_stack_bottom);
    }
    if (info->host_fiber) {
      info->host_fiber->Destroy();
    }
    runtime.thread_registry().UnregisterFiber(fiber_addr);
  }
  mem->SystemHeapFree(fiber_addr);
}

REX_SHIM_FN(SwitchToFiber, void, mapped_void lpFiber) {
  auto* thread = ThreadRecord::GetCurrentThread();
  Runtime& runtime = thread->runtime();
  auto* mem = runtime.memory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  u32 target_addr = lpFiber.guest_address();

  // Validate target
  auto target_info = runtime.thread_registry().LookupFiber(target_addr);
  if (!target_info || !target_info->host_fiber) {
    REXLOG_WARN(kernel, "SwitchToFiber: no valid fiber for {:#010x}, skipping", target_addr);
    return;
  }

  // Save outgoing fiber's non-volatile registers and SP
  u32 current_addr = static_cast<u32>(kthread->fiber_ptr);
  if (current_addr) {
    auto* current_fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(current_addr);
    ctx->SaveNonVolatiles(current_fiber->register_save_area);
    current_fiber->sp_save = ctx->r1.u64;
    current_fiber->cr_save = ctx->cr();
    current_fiber->lr_save = static_cast<u32>(ctx->lr);
  }

  // Set target as active
  kthread->fiber_ptr = target_addr;

  // Restore target's non-volatile registers and guest stack state
  auto* target_fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(target_addr);
  ctx->RestoreNonVolatiles(target_fiber->register_save_area);
  ctx->set_cr(target_fiber->cr_save);
  ctx->lr = target_fiber->lr_save;
  UpdateGuestStackPointers(kthread, pcr, ctx, static_cast<u32>(target_fiber->sp_save),
                           static_cast<u32>(target_fiber->stack_alloc_base),
                           static_cast<u32>(target_fiber->stack_base),
                           static_cast<u32>(target_fiber->stack_limit));

  // profiling
  if (target_info->host_fiber == thread->main_fiber()) {
    REX_PROFILE_FIBER_LEAVE();
  } else {
    REX_PROFILE_FIBER_ENTER(
        runtime.thread_registry().GetOrCreateFiberName(target_info->uid, thread->name().c_str()));
  }

  // Host fiber switch -- suspends here, resumes when switched back
  rex::platform::Fiber::SwitchTo(target_info->host_fiber);
  ctx->fpscr.setcsr(ctx->fpscr.csr);
}

}  // namespace rex::kernel::shim
