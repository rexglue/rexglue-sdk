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

#include <algorithm>
#include <cstring>
#include <memory>
#include <tuple>

#include <rex/platform/exceptions.h>
#include <rex/dbg.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/kernel/shim/function.h>
#include <rex/runtime/thread_registry.h>
#include <rex/types.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xstatus.h>
#include <rex/runtime/process.h>
#include <rex/platform/fiber.h>
#include "runtime/guest_stack.h"

namespace rex::kernel::shim {
using namespace rex::runtime;

namespace {

std::atomic<u32> g_unique_fiber_count;

struct GuestThreadPtrs {
  X_KTHREAD* kthread;
  X_KPCR* pcr;
  PPCContext* ctx;
  runtime::Memory* mem;
};

/**
 * Returns the KTHREAD, PCR and context pointers of the thread.
 *
 * @param thread The thread.
 * @return The resolved pointers.
 */
GuestThreadPtrs GetGuestThreadPtrs(KernelThread* thread) {
  auto* ctx = thread->GetThreadState()->GetContext();
  auto* mem = thread->GetRuntime().GetMemory();
  auto* pcr = mem->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  auto* kthread =
      mem->TranslateVirtual<X_KTHREAD*>(static_cast<u32>(pcr->prcb_data.current_thread));
  return {kthread, pcr, ctx, mem};
}

/**
 * Points the KTHREAD, PCR and context at a stack.
 *
 * @param kthread The KTHREAD to update.
 * @param pcr The PCR to update.
 * @param ctx The context whose r1 receives the stack pointer.
 * @param sp The stack pointer.
 * @param stack_alloc_base The base of the stack allocation.
 * @param stack_base The top of the stack.
 * @param stack_limit The bottom of the stack.
 */
void UpdateGuestStackPointers(X_KTHREAD* kthread, X_KPCR* pcr, PPCContext* ctx, u32 sp,
                              u32 stack_alloc_base, u32 stack_base, u32 stack_limit) {
  kthread->stack_alloc_base = stack_alloc_base;
  kthread->stack_base = stack_base;
  kthread->stack_limit = stack_limit;
  pcr->stack_base_ptr = stack_base;
  pcr->stack_end_ptr = stack_limit;
  ctx->r1.u64 = sp;
}

struct FiberEntryArgs {
  PPCFunc* start_fn;
  u32 guest_fiber_addr;
  Runtime* runtime;
};

static void FiberEntryPoint(void* raw_arg) {
  auto args = std::unique_ptr<FiberEntryArgs>(static_cast<FiberEntryArgs*>(raw_arg));
  auto* thread = KernelThread::GetCurrentThread();
  auto* mem = args->runtime->GetMemory();
  PPCContext& ctx = *thread->GetThreadState()->GetContext();
  ctx.fpscr.SetCSR(ctx.fpscr.csr);

  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(args->guest_fiber_addr);
  ctx.r3.u64 = static_cast<u32>(fiber->fiber_data);
#if !REX_PLATFORM_WIN32
  rex::platform::GetSEHThreadState().top = nullptr;
#endif

  try {
    args->start_fn(ctx, mem->GetVirtualMembase());
  } catch (const rex::ThreadStop& stop) {
    thread->RequestStop(stop.exit_code);
  }

  if (thread->GetMainFiber()) {
    REX_PROFILE_FIBER_LEAVE();
    rex::platform::Fiber::SwitchTo(thread->GetMainFiber());
  }
  REXLOG_WARN(kernel, "FiberEntryPoint: fiber function returned with no main fiber, terminating");
  std::terminate();
}

/**
 * Converts the fiber of the thread back to a thread, shared by ConvertFiberToThread and
 * DeleteFiber for the self delete case.
 *
 * @param thread The thread.
 * @return 1 on success, 0 when the thread is not a fiber.
 */
u32 ConvertFiberToThreadImpl(KernelThread* thread) {
  Runtime& runtime = thread->GetRuntime();
  auto [kthread, pcr, ctx, mem] = GetGuestThreadPtrs(thread);

  u32 fiber_addr = kthread->fiber_ptr;
  if (!fiber_addr) {
    // ERROR_ALREADY_THREAD
    kthread->last_error = 0x501;
    return 0;
  }

  auto info = runtime.GetThreadRegistry().FindFiber(fiber_addr);
  if (info) {
    info->host_fiber->Destroy();
    runtime.GetThreadRegistry().UnregisterFiber(fiber_addr);
  }

  kthread->fiber_ptr = 0u;
  // Null main_fiber_ after Destroy, ~KernelThread destroys it again when non null.
  thread->SetMainFiber(nullptr);
  mem->SystemHeapFree(fiber_addr);

  return 1;
}

}  // namespace

REX_SHIM_FN(ConvertThreadToFiber, u32, MappedVoid lpParameter) {
  auto* thread = KernelThread::GetCurrentThread();
  Runtime& runtime = thread->GetRuntime();
  auto [kthread, pcr, ctx, mem] = GetGuestThreadPtrs(thread);

  if (kthread->fiber_ptr) {
    // ERROR_ALREADY_FIBER
    kthread->last_error = 0x500;
    return 0;
  }

  u32 buf_addr = mem->SystemHeapAlloc(sizeof(X_FIBER_CONTEXT));
  if (!buf_addr) {
    // ERROR_NOT_ENOUGH_MEMORY
    kthread->last_error = 8;
    return 0;
  }
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(buf_addr);
  std::memset(fiber, 0, sizeof(X_FIBER_CONTEXT));

  fiber->fiber_data = lpParameter.GetGuestAddress();
  fiber->stack_alloc_base = kthread->stack_alloc_base;
  fiber->stack_base = kthread->stack_base;
  fiber->stack_limit = kthread->stack_limit;
  fiber->sp_save = ctx->r1.u64;

  kthread->fiber_ptr = buf_addr;

  auto* host_fiber = thread->GetMainFiber();
  if (!host_fiber) {
    host_fiber = rex::platform::Fiber::ConvertCurrentThread();
    thread->SetMainFiber(host_fiber);
  }

  runtime.GetThreadRegistry().RegisterFiber(
      buf_addr, GuestFiber{host_fiber, g_unique_fiber_count++, buf_addr, 0, true});

  REXLOG_DEBUG(kernel, "ConvertThreadToFiber: fiber={:#010x} param={:#010x}", buf_addr,
               lpParameter.GetGuestAddress());
  return buf_addr;
}

REX_SHIM_FN(ConvertFiberToThread, u32) {
  auto* thread = KernelThread::GetCurrentThread();
  auto result = ConvertFiberToThreadImpl(thread);
  if (result) {
    REXLOG_DEBUG(kernel, "ConvertFiberToThread: success");
  }
  return result;
}

REX_SHIM_FN(CreateFiber, u32, u32 dwStackSize, u32 lpStartAddress, MappedVoid lpParameter) {
  auto* thread = KernelThread::GetCurrentThread();
  Runtime& runtime = thread->GetRuntime();
  auto* mem = runtime.GetMemory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);

  const auto* process =
      mem->TranslateVirtual<const X_KPROCESS*>(static_cast<u32>(kthread->process));
  const u32 guest_stack_size =
      ResolveGuestStackSize(dwStackSize, process->kernel_stack_size, kMinimumFiberStackSize);
  Result<GuestStack> stack = AllocateGuestStack(*mem, guest_stack_size, 0, true);
  if (!stack) {
    // ERROR_NOT_ENOUGH_MEMORY
    kthread->last_error = 8;
    return 0;
  }
  const u32 stack_top = stack->base;
  const u32 stack_bottom = stack->limit;
  const u32 initial_sp = stack_top - 0x50;
  std::memset(mem->TranslateVirtual(initial_sp), 0, 0x50);

  PPCFunc* start_fn = runtime.GetFunctionDispatcher()->GetFunction(lpStartAddress);

  u32 buf_addr = mem->SystemHeapAlloc(sizeof(X_FIBER_CONTEXT));
  if (!buf_addr) {
    FreeGuestStack(*mem, stack->alloc_base);
    kthread->last_error = 8;
    return 0;
  }
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(buf_addr);
  std::memset(fiber, 0, sizeof(X_FIBER_CONTEXT));

  fiber->fiber_data = lpParameter.GetGuestAddress();
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

  runtime.GetThreadRegistry().RegisterFiber(
      buf_addr, GuestFiber{host_fiber, g_unique_fiber_count++, buf_addr, stack->alloc_base, false});

  REXLOG_DEBUG(kernel, "CreateFiber: fiber={:#010x} start={:#010x} stack={:#x} param={:#010x}",
               buf_addr, lpStartAddress, guest_stack_size, lpParameter.GetGuestAddress());
  return buf_addr;
}

REX_SHIM_FN(DeleteFiber, void, MappedVoid lpFiber) {
  auto* thread = KernelThread::GetCurrentThread();
  Runtime& runtime = thread->GetRuntime();
  auto* mem = runtime.GetMemory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  u32 fiber_addr = lpFiber.GetGuestAddress();

  if (static_cast<u32>(kthread->fiber_ptr) == fiber_addr) {
    REXLOG_DEBUG(kernel, "DeleteFiber: self delete fiber={:#010x}", fiber_addr);
    REX_PROFILE_FIBER_LEAVE();
    ConvertFiberToThreadImpl(thread);
    // Exit does not return
    thread->Exit(1);
    return;
  }

  REXLOG_DEBUG(kernel, "DeleteFiber: fiber={:#010x}", fiber_addr);

  auto info = runtime.GetThreadRegistry().FindFiber(fiber_addr);
  if (info) {
    if (!info->is_thread_fiber) {
      FreeGuestStack(*mem, info->guest_stack_bottom);
    }
    if (info->host_fiber) {
      info->host_fiber->Destroy();
    }
    runtime.GetThreadRegistry().UnregisterFiber(fiber_addr);
  }
  mem->SystemHeapFree(fiber_addr);
}

REX_SHIM_FN(SwitchToFiber, void, MappedVoid lpFiber) {
  auto* thread = KernelThread::GetCurrentThread();
  Runtime& runtime = thread->GetRuntime();
  auto* mem = runtime.GetMemory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  u32 target_addr = lpFiber.GetGuestAddress();

  auto target_info = runtime.GetThreadRegistry().FindFiber(target_addr);
  if (!target_info || !target_info->host_fiber) {
    REXLOG_WARN(kernel, "SwitchToFiber: no valid fiber for {:#010x}, skipping", target_addr);
    return;
  }

  u32 current_addr = static_cast<u32>(kthread->fiber_ptr);
  if (current_addr) {
    auto* current_fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(current_addr);
    ctx->SaveNonVolatiles(current_fiber->register_save_area);
    current_fiber->sp_save = ctx->r1.u64;
    current_fiber->cr_save = ctx->GetCR();
    current_fiber->lr_save = static_cast<u32>(ctx->lr);
  }

  kthread->fiber_ptr = target_addr;

  auto* target_fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(target_addr);
  ctx->RestoreNonVolatiles(target_fiber->register_save_area);
  ctx->SetCR(target_fiber->cr_save);
  ctx->lr = target_fiber->lr_save;
  UpdateGuestStackPointers(kthread, pcr, ctx, static_cast<u32>(target_fiber->sp_save),
                           static_cast<u32>(target_fiber->stack_alloc_base),
                           static_cast<u32>(target_fiber->stack_base),
                           static_cast<u32>(target_fiber->stack_limit));

  if (target_info->host_fiber == thread->GetMainFiber()) {
    REX_PROFILE_FIBER_LEAVE();
  } else {
    REX_PROFILE_FIBER_ENTER(runtime.GetThreadRegistry().GetOrCreateFiberName(
        target_info->uid, thread->GetName().c_str()));
  }

#if !REX_PLATFORM_WIN32
  rex::platform::SEHScope* const scope = rex::platform::GetSEHThreadState().top;
#endif
  rex::platform::Fiber::SwitchTo(target_info->host_fiber);
#if !REX_PLATFORM_WIN32
  rex::platform::GetSEHThreadState().top = scope;
#endif
  ctx->fpscr.SetCSR(ctx->fpscr.csr);
  KernelThread::CheckStop();
}

}  // namespace rex::kernel::shim
