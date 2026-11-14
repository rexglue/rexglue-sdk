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

#include <rex/kernel/types/thread.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/seh.h>
#include <rex/platform/perf.h>
#include <rex/logging/log_category.h>
#include "kernel/categories.h"
#include <rex/ppc/hook.h>
#include "kernel/shim/function.h"
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include <rex/platform/thread.h>
#include "kernel/guest_stack.h"
#include <rex/kernel/thread_state.h>
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::shim {
using namespace rex::kernel;

namespace {

struct GuestThreadPtrs {
  X_KTHREAD* kthread;
  X_KPCR* pcr;
  PPCContext* ctx;
  kernel::GuestAddressSpace* mem;
};

GuestThreadPtrs GetGuestThreadPtrs(KernelThread* thread) {
  auto* ctx = thread->GetThreadState()->GetContext();
  auto* mem = StateOf(thread->GetRuntime()).GetAddressSpace();
  auto* pcr = mem->TranslateVirtual<X_KPCR*>(static_cast<u32>(ctx->r13.u64));
  auto* kthread =
      mem->TranslateVirtual<X_KTHREAD*>(static_cast<u32>(pcr->prcb_data.current_thread));
  return {kthread, pcr, ctx, mem};
}

void UpdateGuestStackPointers(X_KTHREAD* kthread, X_KPCR* pcr, PPCContext* ctx, u32 sp,
                              u32 stack_alloc_base, u32 stack_base, u32 stack_limit) {
  kthread->stack_alloc_base = stack_alloc_base;
  kthread->stack_base = stack_base;
  kthread->stack_limit = stack_limit;
  pcr->stack_base_ptr = stack_base;
  pcr->stack_end_ptr = stack_limit;
  ctx->r1.u64 = sp;
}

struct FiberEntryData {
  PPCFunc* start_fn;
  u32 guest_fiber_addr;
  TitleRuntime* runtime;
};

static void FiberEntryPoint(void* raw_arg) {
  auto args = std::unique_ptr<FiberEntryData>(static_cast<FiberEntryData*>(raw_arg));
  auto* thread = KernelThread::GetCallingThread();
  auto* mem = StateOf(*args->runtime).GetAddressSpace();
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
  REXKRNL_WARN("FiberEntryPoint: fiber function returned with no main fiber, terminating");
  std::terminate();
}

/**
 * Converts the fiber of a thread back to a plain thread.
 *
 * @return 1 on success, 0 when the thread is not a fiber.
 */
u32 ResetThreadFiber(KernelThread* thread) {
  TitleRuntime& runtime = thread->GetRuntime();
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
  // Cleared after Destroy so ~KernelThread does not destroy it again.
  thread->SetMainFiber(nullptr);
  mem->SystemHeapFree(fiber_addr);

  return 1;
}

}  // namespace

REX_SHIM_FN(ConvertThreadToFiber, u32, MappedVoid lpParameter) {
  auto* thread = KernelThread::GetCallingThread();
  TitleRuntime& runtime = thread->GetRuntime();
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
      buf_addr, GuestFiber{host_fiber, kernel::GetCurrentState().fiber_count++, buf_addr, 0, true});

  REXKRNL_DEBUG("ConvertThreadToFiber: fiber={:#010x} param={:#010x}", buf_addr,
                lpParameter.GetGuestAddress());
  return buf_addr;
}

REX_SHIM_FN(ConvertFiberToThread, u32) {
  auto* thread = KernelThread::GetCallingThread();
  auto result = ResetThreadFiber(thread);
  if (result) {
    REXKRNL_DEBUG("ConvertFiberToThread: success");
  }
  return result;
}

REX_SHIM_FN(CreateFiber, u32, u32 dwStackSize, u32 lpStartAddress, MappedVoid lpParameter) {
  auto* thread = KernelThread::GetCallingThread();
  TitleRuntime& runtime = thread->GetRuntime();
  auto* mem = StateOf(runtime).GetAddressSpace();
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

  PPCFunc* start_fn = StateOf(runtime).GetFunctionDispatcher()->GetFunction(lpStartAddress);

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
  auto args_owner = std::make_unique<FiberEntryData>(FiberEntryData{
      start_fn,
      buf_addr,
      &runtime,
  });
  auto* host_fiber = rex::platform::Fiber::Create(host_stack, FiberEntryPoint, args_owner.get());
  if (host_fiber) {
    std::ignore = args_owner.release();
  }

  runtime.GetThreadRegistry().RegisterFiber(
      buf_addr, GuestFiber{host_fiber, kernel::GetCurrentState().fiber_count++, buf_addr,
                           stack->alloc_base, false});

  REXKRNL_DEBUG("CreateFiber: fiber={:#010x} start={:#010x} stack={:#x} param={:#010x}", buf_addr,
                lpStartAddress, guest_stack_size, lpParameter.GetGuestAddress());
  return buf_addr;
}

REX_SHIM_FN(DeleteFiber, void, MappedVoid lpFiber) {
  auto* thread = KernelThread::GetCallingThread();
  TitleRuntime& runtime = thread->GetRuntime();
  auto* mem = StateOf(runtime).GetAddressSpace();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  u32 fiber_addr = lpFiber.GetGuestAddress();

  if (static_cast<u32>(kthread->fiber_ptr) == fiber_addr) {
    REXKRNL_DEBUG("DeleteFiber: self delete fiber={:#010x}", fiber_addr);
    REX_PROFILE_FIBER_LEAVE();
    ResetThreadFiber(thread);
    thread->Exit(1);
    return;
  }

  REXKRNL_DEBUG("DeleteFiber: fiber={:#010x}", fiber_addr);

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
  auto* thread = KernelThread::GetCallingThread();
  TitleRuntime& runtime = thread->GetRuntime();
  auto* mem = StateOf(runtime).GetAddressSpace();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  u32 target_addr = lpFiber.GetGuestAddress();

  auto target_info = runtime.GetThreadRegistry().FindFiber(target_addr);
  if (!target_info || !target_info->host_fiber) {
    REXKRNL_WARN("SwitchToFiber: no valid fiber for {:#010x}, skipping", target_addr);
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
