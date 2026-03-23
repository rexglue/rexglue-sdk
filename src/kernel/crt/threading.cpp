/**
 * @file        kernel/crt/threading.cpp
 * @brief       rexcrt hooks for XAPI fiber functions
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

#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread/fiber.h>

namespace rex::kernel::crt {
using namespace rex::system;

namespace {

std::atomic<uint32_t> unique_fiber_count;

/// get KTHREAD and PCR pointers from the current thread's context.
struct GuestThreadPtrs {
  X_KTHREAD* kthread;
  X_KPCR* pcr;
  PPCContext* ctx;
  memory::Memory* mem;
};

GuestThreadPtrs GetGuestThreadPtrs(XThread* thread) {
  auto* ctx = thread->thread_state()->context();
  auto* mem = thread->kernel_state()->memory();
  auto* pcr = mem->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  auto* kthread =
      mem->TranslateVirtual<X_KTHREAD*>(static_cast<uint32_t>(pcr->prcb_data.current_thread));
  return {kthread, pcr, ctx, mem};
}

/// update KTHREAD/PCR/ctx stack pointers
void UpdateGuestStackPointers(X_KTHREAD* kthread, X_KPCR* pcr, PPCContext* ctx, uint32_t sp,
                              uint32_t stack_alloc_base, uint32_t stack_base,
                              uint32_t stack_limit) {
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
  uint32_t guest_fiber_addr;
  KernelState* kernel_state;
};

static void FiberEntryPoint(void* raw_arg) {
  auto args = std::unique_ptr<FiberEntryArgs>(static_cast<FiberEntryArgs*>(raw_arg));
  auto* thread = XThread::GetCurrentThread();
  auto* mem = args->kernel_state->memory();
  PPCContext& ctx = *thread->thread_state()->context();

  // Read fiber_data (lpParameter) from guest context buffer
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(args->guest_fiber_addr);
  ctx.r3.u64 = static_cast<uint32_t>(fiber->fiber_data);

  // Call the fiber function
  args->start_fn(ctx, mem->virtual_membase());

  // Fiber returned (shouldn't per XDK) -- safe fallback
  if (thread->main_fiber()) {
    PROFILE_FIBER_LEAVE;
    rex::thread::Fiber::SwitchTo(thread->main_fiber());
  }
  REXKRNL_WARN("FiberEntryPoint: fiber function returned with no main fiber - terminating");
  std::terminate();
}

/// Helper for ConvertFiberToThread logic, shared by ConvertFiberToThread_entry
/// and DeleteFiber_entry (self-delete case).
ppc_u32_result_t ConvertFiberToThread_impl(XThread* thread) {
  auto* ks = thread->kernel_state();
  auto [kthread, pcr, ctx, mem] = GetGuestThreadPtrs(thread);

  uint32_t fiber_addr = kthread->fiber_ptr;
  if (!fiber_addr) {
    kthread->last_error = 0x501;  // ERROR_ALREADY_THREAD
    return 0;
  }

  auto* info = ks->LookupFiber(fiber_addr);
  if (info) {
    info->host_fiber->Destroy();
    ks->UnregisterFiber(fiber_addr);
  }

  kthread->fiber_ptr = 0u;
  // Must null main_fiber_ AFTER Destroy() above to avoid double-free
  // in ~XThread, which also calls main_fiber_->Destroy() if non-null.
  thread->set_main_fiber(nullptr);
  mem->SystemHeapFree(fiber_addr);

  return 1;  // TRUE
}

}  // namespace

//=============================================================================
// XAPI Fiber Function Implementations
//=============================================================================

ppc_ptr_result_t ConvertThreadToFiber_entry(ppc_pvoid_t lpParameter) {
  auto* thread = XThread::GetCurrentThread();
  auto* ks = thread->kernel_state();
  auto [kthread, pcr, ctx, mem] = GetGuestThreadPtrs(thread);

  if (kthread->fiber_ptr) {
    kthread->last_error = 0x500;  // ERROR_ALREADY_FIBER
    return 0;
  }

  // Allocate guest fiber context buffer
  uint32_t buf_addr = mem->SystemHeapAlloc(sizeof(X_FIBER_CONTEXT));
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

  // Reuse existing host fiber from XThread::Execute() if available;
  // otherwise create one (should not happen in normal flow).
  auto* host_fiber = thread->main_fiber();
  if (!host_fiber) {
    host_fiber = rex::thread::Fiber::ConvertCurrentThread();
    thread->set_main_fiber(host_fiber);
  }

  ks->RegisterFiber(buf_addr, FiberInfo{host_fiber, unique_fiber_count++, buf_addr, 0, 0, true});

  REXKRNL_DEBUG("ConvertThreadToFiber: fiber={:#010x} param={:#010x}", buf_addr,
                lpParameter.guest_address());
  return buf_addr;
}

ppc_u32_result_t ConvertFiberToThread_entry() {
  auto* thread = XThread::GetCurrentThread();
  auto result = ConvertFiberToThread_impl(thread);
  if (result) {
    REXKRNL_DEBUG("ConvertFiberToThread: success");
  }
  return result;
}

ppc_ptr_result_t CreateFiber_entry(ppc_u32_t dwStackSize, ppc_fn_t lpStartAddress,
                                   ppc_pvoid_t lpParameter) {
  auto* thread = XThread::GetCurrentThread();
  auto* ks = thread->kernel_state();
  auto* mem = ks->memory();

  // Determine guest stack size
  uint32_t guest_stack_size = dwStackSize.value();
  if (guest_stack_size == 0)
    guest_stack_size = 0x10000;                            // 64KB default
  guest_stack_size = (guest_stack_size + 0xFFF) & ~0xFFF;  // page-align
  if (guest_stack_size < 0x4000)
    guest_stack_size = 0x4000;  // 16KB minimum

  // Allocate guest kernel stack (for PPC stack variables via ctx.r1).
  // guest_stack_size is already page-aligned.
  uint32_t stack_alignment = (guest_stack_size & 0xF000) ? 0x1000 : 0x10000;
  uint32_t stack_address = 0;
  mem->LookupHeap(0x70000000)
      ->AllocRange(0x70000000, 0x7F000000, guest_stack_size, stack_alignment,
                   memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                   memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false, &stack_address);
  if (!stack_address) {
    auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
    kthread->last_error = 8;  // ERROR_NOT_ENOUGH_MEMORY
    return 0;
  }
  uint32_t stack_top = stack_address + guest_stack_size;
  uint32_t stack_bottom = stack_address;
  uint32_t initial_sp = stack_top - 0x50;

  // Zero the initial 80-byte frame
  std::memset(mem->TranslateVirtual(initial_sp), 0, 0x50);

  // Resolve start address to host function pointer
  PPCFunc* start_fn = ks->function_dispatcher()->GetFunction(lpStartAddress.value());

  // Allocate guest fiber context buffer
  uint32_t buf_addr = mem->SystemHeapAlloc(sizeof(X_FIBER_CONTEXT));
  if (!buf_addr) {
    mem->LookupHeap(0x70000000)->Release(stack_address);
    auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
    kthread->last_error = 8;
    return 0;
  }
  auto* fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(buf_addr);
  std::memset(fiber, 0, sizeof(X_FIBER_CONTEXT));

  fiber->fiber_data = lpParameter.guest_address();
  fiber->stack_alloc_base = stack_top;
  fiber->stack_base = stack_top;  // = alloc_base initially
  fiber->stack_limit = stack_bottom;
  fiber->sp_save = initial_sp;

  // Create host fiber
  size_t host_stack =
      std::max(static_cast<size_t>(guest_stack_size), static_cast<size_t>(256u * 1024u));
  auto args_owner = std::make_unique<FiberEntryArgs>(FiberEntryArgs{
      start_fn,
      buf_addr,
      ks,
  });
  auto* host_fiber = rex::thread::Fiber::Create(host_stack, FiberEntryPoint, args_owner.get());
  if (host_fiber) {
    args_owner.release();  // FiberEntryPoint takes ownership
  }

  ks->RegisterFiber(buf_addr, FiberInfo{host_fiber, unique_fiber_count++, buf_addr, stack_top,
                                        stack_bottom, false});

  REXKRNL_DEBUG("CreateFiber: fiber={:#010x} start={:#010x} stack={:#x} param={:#010x}", buf_addr,
                lpStartAddress.value(), guest_stack_size, lpParameter.guest_address());
  return buf_addr;
}

void DeleteFiber_entry(ppc_pvoid_t lpFiber) {
  auto* thread = XThread::GetCurrentThread();
  auto* ks = thread->kernel_state();
  auto* mem = ks->memory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  uint32_t fiber_addr = lpFiber.guest_address();

  // Self-delete: ConvertFiberToThread + ExitThread
  if (static_cast<uint32_t>(kthread->fiber_ptr) == fiber_addr) {
    REXKRNL_DEBUG("DeleteFiber: self-delete fiber={:#010x}", fiber_addr);
    PROFILE_FIBER_LEAVE;
    ConvertFiberToThread_impl(thread);
    thread->Exit(1);  // does not return
    return;
  }

  REXKRNL_DEBUG("DeleteFiber: fiber={:#010x}", fiber_addr);

  auto* info = ks->LookupFiber(fiber_addr);
  if (info) {
    if (!info->is_thread_fiber && info->guest_stack_bottom) {
      mem->LookupHeap(0x70000000)->Release(info->guest_stack_bottom);
    }
    if (info->host_fiber) {
      info->host_fiber->Destroy();
    }
    ks->UnregisterFiber(fiber_addr);
  }
  mem->SystemHeapFree(fiber_addr);
}

void SwitchToFiber_entry(ppc_pvoid_t lpFiber) {
  auto* thread = XThread::GetCurrentThread();
  auto* ks = thread->kernel_state();
  auto* mem = ks->memory();
  auto [kthread, pcr, ctx, _mem] = GetGuestThreadPtrs(thread);
  uint32_t target_addr = lpFiber.guest_address();

  // Validate target
  auto* target_info = ks->LookupFiber(target_addr);
  if (!target_info || !target_info->host_fiber) {
    REXKRNL_WARN("SwitchToFiber: no valid fiber for {:#010x}, skipping", target_addr);
    return;
  }

  // Save outgoing fiber's non-volatile registers and SP
  uint32_t current_addr = static_cast<uint32_t>(kthread->fiber_ptr);
  if (current_addr) {
    auto* current_fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(current_addr);
    ctx->SaveNonVolatiles(current_fiber->register_save_area);
    current_fiber->sp_save = ctx->r1.u64;
  }

  // Set target as active
  kthread->fiber_ptr = target_addr;

  // Restore target's non-volatile registers and guest stack state
  auto* target_fiber = mem->TranslateVirtual<X_FIBER_CONTEXT*>(target_addr);
  ctx->RestoreNonVolatiles(target_fiber->register_save_area);
  UpdateGuestStackPointers(kthread, pcr, ctx, static_cast<uint32_t>(target_fiber->sp_save),
                           static_cast<uint32_t>(target_fiber->stack_alloc_base),
                           static_cast<uint32_t>(target_fiber->stack_base),
                           static_cast<uint32_t>(target_fiber->stack_limit));

  // profiling
  if (target_info->host_fiber == thread->main_fiber()) {
    PROFILE_FIBER_LEAVE;
  } else {
    PROFILE_FIBER_ENTER(ks->GetOrCreateFiberName(target_info->uid, thread->name().c_str()));
  }

  // Host fiber switch -- suspends here, resumes when switched back
  rex::thread::Fiber::SwitchTo(target_info->host_fiber);
  // Resumed: non-volatile regs and stack state already restored by the fiber that switched back.
}

}  // namespace rex::kernel::crt

//=============================================================================
// REXCRT_EXPORT wiring
//=============================================================================

REXCRT_EXPORT(rexcrt_ConvertThreadToFiber, rex::kernel::crt::ConvertThreadToFiber_entry)
REXCRT_EXPORT(rexcrt_ConvertFiberToThread, rex::kernel::crt::ConvertFiberToThread_entry)
REXCRT_EXPORT(rexcrt_CreateFiber, rex::kernel::crt::CreateFiber_entry)
REXCRT_EXPORT(rexcrt_DeleteFiber, rex::kernel::crt::DeleteFiber_entry)
REXCRT_EXPORT(rexcrt_SwitchToFiber, rex::kernel::crt::SwitchToFiber_entry)
