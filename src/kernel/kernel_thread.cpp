/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <format>
#include <vector>

#include <rex/chrono.h>
#include <rex/platform/perf.h>
#include <rex/logging/log.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/types/thread.h>
#include <rex/types.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/seh.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/xboxkrnl.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/ppc/vec128.h>
#include <rex/utility.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>
#include "kernel/delay_observer.h"
#include "kernel/thread_registry.h"
#include "kernel/guest_stack.h"
#include "kernel/settings_access.h"
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

thread_local std::unique_ptr<rex::platform::Thread> t_exiting_thread;

}  // namespace

struct KernelThread::SuspendWait {
  std::mutex mutex;
  std::condition_variable cv;
};

KernelThread::KernelThread(TitleRuntime& runtime, u32 stack_size, u32 xapi_thread_startup,
                           u32 start_address, u32 start_context, u32 creation_flags,
                           bool guest_thread, bool main_thread, u32 title_process)
    : guest_thread_(guest_thread),
      main_thread_(main_thread),
      suspend_wait_(std::make_unique<SuspendWait>()),
      runtime_(&runtime) {
  creation_params_.stack_size = stack_size;
  creation_params_.xapi_thread_startup = xapi_thread_startup;
  creation_params_.start_address = start_address;
  creation_params_.start_context = start_context;

  // Bits 24..29 hold the processor mask and bit 0 creates suspended.
  creation_params_.creation_flags = creation_flags;
  creation_params_.title_process = title_process;
}

KernelThread::KernelThread(TitleRuntime& runtime, u32 stack_size, u32 creation_flags,
                           rex::MoveOnlyFunction<int()> host_fn)
    : KernelThread(runtime, stack_size, 0, 0, 0, creation_flags, false) {
  host_fn_ = std::move(host_fn);
}

KernelThread::~KernelThread() {
  if (main_fiber_) {
    main_fiber_->Destroy();
    main_fiber_ = nullptr;
  }

  if (thread_ && thread_.get() == rex::platform::Thread::GetCallingThread()) {
    t_exiting_thread = std::move(thread_);
  }
  thread_.reset();

  StateOf(*runtime_).GetMemory()->SystemHeapFree(scratch_address_);
  FreeStack();
  StateOf(*runtime_).GetMemory()->SystemHeapFree(pcr_address_);
  FreeObjectBlock(GetMemory(), guest_object_ptr_);
}

kernel::Memory* KernelThread::GetMemory() const {
  return StateOf(*runtime_).GetMemory();
}

u32 KernelThread::GetProcessAddress() const {
  return creation_params_.title_process ? creation_params_.title_process
                                        : StateOf(*runtime_).GetProcessInfoBlockAddress();
}

thread_local KernelThread* current_thread_tls = nullptr;

namespace {

KernelThread* GetBoundCurrentThread() {
  return current_thread_tls;
}

}  // namespace

bool KernelThread::IsInThread() {
  return GetBoundCurrentThread() != nullptr;
}

bool KernelThread::IsInThread(const KernelThread* other) {
  return GetBoundCurrentThread() == other;
}

KernelThread* KernelThread::GetCallingThread() {
  KernelThread* thread = GetBoundCurrentThread();
  if (!thread) {
    REX_ASSERT_ALWAYS("Attempting to use kernel APIs from a non-kernel thread");
  }
  return thread;
}

void KernelThread::CheckStop() {
  KernelThread* self = GetBoundCurrentThread();
  if (!self || self->exiting_ || !self->IsRunning()) {
    return;
  }
  if (self->suspend_deferred_.load(std::memory_order_acquire)) {
    self->TakeDeferredSuspend();
  }
  if (self->thread_state_->IsStopRequested() ||
      (self->IsGuestThread() && StateOf(self->GetRuntime()).IsTerminatingTitle())) {
    throw rex::ThreadStop{self->stop_exit_code_.load(std::memory_order_acquire)};
  }
}

void KernelThread::ThrowStop() {
  throw rex::ThreadStop{GetCallingThread()->stop_exit_code_.load(std::memory_order_acquire)};
}

bool KernelThread::ForceStop() {
  rex::platform::Thread* thread = nullptr;
  {
    std::lock_guard lock(stop_mutex_);
    thread = thread_.get();
  }
  if (!thread) {
    return false;
  }
  while (GetSuspendCount() > 0 && Resume() == X_STATUS_SUCCESS) {}
  const auto in_code = [](u64 pc, void* context) {
    const auto* thread = static_cast<KernelThread*>(context);
    return !thread->exiting_ && thread->thread_state_->IsStopRequested() &&
           rex::ppc::IsRecompiledCode(pc);
  };
  return thread->Redirect(in_code, this, &ThrowStop);
}

void KernelThread::SleepFor(std::chrono::microseconds duration) {
  if (!GetBoundCurrentThread()) {
    rex::platform::SleepFor(duration);
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + duration;
  CheckStop();
  for (auto left = duration; left.count() > 0;
       left = std::chrono::duration_cast<std::chrono::microseconds>(
           deadline - std::chrono::steady_clock::now())) {
    (void)rex::platform::AlertableSleep(left);
    CheckStop();
  }
}

void KernelThread::RequestStop(i32 exit_code) {
  stop_exit_code_.store(exit_code, std::memory_order_release);
  std::lock_guard lock(stop_mutex_);
  stop_requested_ = true;
  if (thread_state_) {
    thread_state_->RequestStop();
  }
  if (thread_) {
    thread_->QueueUserCallback([] {});
  }
}

u32 KernelThread::GetCurrentThreadHandle() {
  KernelThread* thread = KernelThread::GetCallingThread();
  return thread->GetHandle();
}

u32 KernelThread::GetLastError() {
  return GetGuestObject<X_KTHREAD>()->last_error;
}

void KernelThread::SetLastError(u32 error_code) {
  GetGuestObject<X_KTHREAD>()->last_error = error_code;
}

void KernelThread::SetName(const std::string_view name) {
  std::lock_guard<std::mutex> lock(thread_lock_);
  thread_name_ = std::format("{} ({:08X})", name, GetHandle());
  crash_name_length_.store(0, std::memory_order_release);
  const auto end =
      std::ranges::copy(std::string_view(thread_name_).substr(0, crash_name_.size() - 1),
                        crash_name_.begin())
          .out;
  crash_name_length_.store(static_cast<u8>(end - crash_name_.begin()), std::memory_order_release);

  if (thread_) {
    thread_->SetName(thread_name_);
  }
}

void KernelThread::InitializeGuestObject() {
  auto kthread = GetGuestObject<X_KTHREAD>();
  u32 guest_ptr = GetGuestObject();

  // Dispatch header type 6 is a thread object.
  kthread->header.type = 6;
  kthread->suspend_count = (creation_params_.creation_flags & kXCreateSuspended) ? 1 : 0;

  kthread->unk_10 = guest_ptr + 0x010;
  kthread->unk_14 = guest_ptr + 0x010;

  kernel::InitializeListHead(&kthread->wait_timeout_block.wait_list_entry, GetMemory());
  kthread->wait_timeout_block.thread = guest_ptr;
  kthread->wait_timeout_block.object = guest_ptr + 0x18;
  kthread->wait_timeout_block.wait_result_xstatus = 0x0100;
  kthread->wait_timeout_block.wait_type = 0x0201;

  kthread->stack_base = stack_base_;
  kthread->stack_limit = stack_limit_;
  kthread->stack_kernel = stack_base_ - 240;
  kthread->tls_address = tls_dynamic_address_;
  kthread->thread_state = 0;

  kthread->apc_lists[0].Initialize(GetMemory());
  kthread->apc_lists[1].Initialize(GetMemory());

  u32 process_ptr = GetProcessAddress();
  kthread->process = process_ptr;
  kthread->may_queue_apcs = 1;

  u32 kpcrb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  kthread->prcb = kpcrb;
  kthread->prcb_alias = kpcrb;

  auto* ctx = thread_state_->GetContext();

  if (process_ptr) {
    auto target_process = GetMemory()->TranslateVirtual<X_KPROCESS*>(process_ptr);
    kthread->process_type = target_process->process_type;
    kthread->process_type_dup = target_process->process_type;

    auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx,
                                                          &target_process->thread_list_spinlock);
    kernel::InsertTailList(&target_process->thread_list, &kthread->process_threads, GetMemory());
    target_process->thread_count = target_process->thread_count + 1;
    kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &target_process->thread_list_spinlock,
                                          old_irql);
  } else {
    kthread->process_type = kXProcTypeUser;
    kthread->process_type_dup = kXProcTypeUser;
  }

  kthread->msr_mask = 0xFDFFD7FF;
  // current_cpu is set by SetActiveCPU.
  kthread->stack_alloc_base = stack_base_;
  kthread->create_time = Clock::QueryGuestSystemTime();

  kernel::InitializeListHead(&kthread->timer_list, GetMemory());

  kthread->thread_id = thread_id_;
  kthread->start_address = creation_params_.start_address;

  kernel::InitializeListHead(&kthread->unk_154, GetMemory());

  kthread->last_error = 0;
  kthread->creation_flags = creation_params_.creation_flags;
  kthread->unk_17c = 1;
}

void KernelThread::FreeStack() {
  if (!stack_alloc_base_) {
    return;
  }
  const u32 globals_address = StateOf(*runtime_).GetKernelGlobals();
  KernelThread* caller = GetBoundCurrentThread();
  KernelThread* owner = caller ? caller : this;
  auto* lock = globals_address
                   ? &GetMemory()->TranslateVirtual<KernelGuestGlobals*>(globals_address)->tls_lock
                   : nullptr;
  if (lock && owner->thread_state_) {
    const bool change_irql = caller != nullptr;
    auto* ctx = owner->thread_state_->GetContext();
    const auto old_irql =
        kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx, lock, change_irql);
    tls_slot_count_.store(0, std::memory_order_release);
    kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, lock, old_irql, change_irql);
  } else {
    tls_slot_count_.store(0, std::memory_order_release);
  }
  FreeGuestStack(*GetMemory(), stack_alloc_base_);
  stack_alloc_base_ = 0;
  stack_base_ = 0;
  stack_limit_ = 0;
}

/**
 * Allocates the stack, TLS and thread state block and creates the suspended host thread.
 *
 * The thread state block is the X_KPCR that r13 points at.
 *
 * @return X_STATUS_SUCCESS, or an error status when allocation or thread creation fails.
 */
X_STATUS KernelThread::Create() {
  const u32 process_address = GetProcessAddress();
  const auto* process =
      process_address ? GetMemory()->TranslateVirtual<const X_KPROCESS*>(process_address) : nullptr;
  const u32 guest_default = process ? static_cast<u32>(process->kernel_stack_size) : 0;
  const u32 data_size = process ? rex::RoundUp(static_cast<u32>(process->tls_data_size), 4u) : 0;
  const u32 slot_count =
      process ? static_cast<u32>(process->tls_slot_size) / 4 : ResolveTLSSlotCount(0);
  const u32 tls_size = data_size + slot_count * 4;
  const u32 stack_size =
      ResolveGuestStackSize(creation_params_.stack_size, guest_default, kMinimumThreadStackSize);
  Result<GuestStack> stack = AllocateGuestStack(*GetMemory(), stack_size, tls_size, guest_thread_);
  if (!stack) {
    return X_STATUS_NO_MEMORY;
  }
  stack_alloc_base_ = stack->alloc_base;
  stack_limit_ = stack->limit;
  stack_base_ = stack->base;

  scratch_size_ = 4 * 16;
  scratch_address_ = GetMemory()->SystemHeapAlloc(scratch_size_);

  tls_static_address_ = stack_base_;
  tls_dynamic_address_ = stack_base_ + data_size;
  GetMemory()->Fill(tls_static_address_, tls_size, 0);
  if (process && process->tls_raw_data_size) {
    GetMemory()->Copy(tls_static_address_, process->tls_static_data_address,
                      std::min(static_cast<u32>(process->tls_raw_data_size), data_size));
  }
  tls_slot_count_.store(slot_count, std::memory_order_release);

  pcr_address_ = GetMemory()->SystemHeapAlloc(0x2D8);
  if (!pcr_address_) {
    REXSYS_WARN("Unable to allocate thread state block");
    return X_STATUS_NO_MEMORY;
  }

  {
    std::lock_guard lock(stop_mutex_);
    thread_state_ = std::make_unique<kernel::ThreadState>(thread_id_, stack_base_, pcr_address_,
                                                          GetMemory(), runtime_);
    if (stop_requested_ || (IsGuestThread() && StateOf(*runtime_).IsTerminatingTitle())) {
      thread_state_->RequestStop();
    }
  }

  const u32 root_frame = stack_base_ - rex::ppc::kLinkageSize;
  GetMemory()->Fill(root_frame, rex::ppc::kLinkageSize, 0);
  thread_state_->GetContext()->r1.u64 = root_frame;

  REXSYS_DEV("KernelThread{:08X} ({:X}) Stack: {:08X}-{:08X}", GetHandle(), thread_id_,
             stack_limit_, stack_base_);

  const u8 processor_mask = static_cast<u8>(creation_params_.creation_flags >> 24);
  u8 cpu_index = 0;
  if (KernelThread* creator = GetBoundCurrentThread(); creator && creator->IsGuestThread()) {
    cpu_index = creator->GetActiveCPU();
  }
  if (processor_mask && !(processor_mask & ~0x3Fu)) {
    cpu_index = static_cast<u8>(std::bit_width(processor_mask) - 1);
  }

  InitializeGuestObject();

  X_KPCR* pcr = GetMemory()->TranslateVirtual<X_KPCR*>(pcr_address_);

  pcr->tls_ptr = tls_static_address_;
  pcr->pcr_ptr = pcr_address_;
  pcr->prcb_data.current_thread = GetGuestObject();
  pcr->prcb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  pcr->host_stash = reinterpret_cast<u64>(thread_state_->GetContext());
  pcr->current_irql = 0;

  pcr->stack_base_ptr = stack_base_;
  pcr->stack_end_ptr = stack_limit_;

  pcr->prcb_data.dpc_active = 0;

  // The thread owns itself until it exits.
  {
    IObjectTable& table = runtime_->GetObjectTable();
    ObjectPin self = table.GetByAddress(guest_object_ptr_);
    if (!self) {
      REXSYS_ERROR("KernelThread{:08X}: its object is not in the table", handle_);
      return X_STATUS_INVALID_HANDLE;
    }
    if (auto referenced = table.Reference(self); !referenced) {
      REXSYS_ERROR("KernelThread{:08X}: cannot take the self reference, {}", handle_,
                   referenced.error().message);
      return ToXStatus(referenced);
    }
  }

  rex::platform::Thread::CreationParameters params;
  params.stack_size = 16_MiB;
  params.create_suspended = true;
  auto thread = rex::platform::Thread::Create(params, [this]() {
    if (abandoned_.load(std::memory_order_acquire)) {
      return;
    }
    rex::InitializeSEHThread();
    kernel::ThreadState::Bind(thread_state_.get());

    rex::platform::SetCallerThreadID(GetHandle());

    thread_->SetName(thread_name_);

    REX_PROFILE_THREAD(thread_name_.c_str());
    REX_PERF_INC("active_threads");

    current_thread_tls = this;
    running_ = true;
    int exit_code = 0;
    try {
      exit_code = Execute();
    } catch (const rex::ThreadStop& stop) {
      exit_code = stop.exit_code;
    }
#if !REX_PLATFORM_WIN32
    rex::platform::GetSEHThreadState().top = nullptr;
#endif
    REX_PERF_ADD("active_threads", -1);
    Finish(exit_code);
  });
  {
    std::lock_guard lock(stop_mutex_);
    thread_ = std::move(thread);
  }

  if (!thread_) {
    ReleaseReference();
    REXSYS_FAIL(X_STATUS_NO_MEMORY, "CreateThread failed");
  }

  if (thread_name_.empty()) {
    SetName(std::format("XThread{:04X}", thread_->GetSystemID()));
  }

  if (creation_params_.creation_flags & 0x60) {
    thread_->SetPriority(creation_params_.creation_flags & 0x20 ? 1 : 0);
  }

  SetActiveCPU(cpu_index);

  return X_STATUS_SUCCESS;
}

void KernelThread::Start() {
  if ((creation_params_.creation_flags & kXCreateSuspended) == 0) {
    thread_->Resume();
  }
}

void KernelThread::Abandon() {
  abandoned_.store(true, std::memory_order_release);
  thread_->Resume();
  rex::platform::Wait(thread_.get(), false, std::chrono::milliseconds::max());
  WriteExitState(X_STATUS_PROCESS_IS_TERMINATING);
}

void KernelThread::WriteKernelTime() {
  KernelThread* self = GetBoundCurrentThread();
  if (!self || !self->guest_object_ptr_) {
    return;
  }
  self->GetGuestObject<X_KTHREAD>()->kernel_time =
      static_cast<u32>(rex::platform::GetCurrentThreadCPUTime().count());
}

void KernelThread::WriteExitState(u32 exit_code) {
  auto* thread = GetGuestObject<X_KTHREAD>();
  thread->terminated = 1;
  thread->thread_state = kKThreadStateTerminated;
  if (IsInThread(this)) {
    WriteKernelTime();
  }
  thread->exit_time = Clock::QueryGuestSystemTime();
  thread->exit_status = exit_code;
  thread->header.signal_state = 1;
  const u32 process_address = thread->process;
  if (!process_address) {
    return;
  }
  KernelThread* caller = GetBoundCurrentThread();
  const bool change_irql = caller != nullptr;
  auto* ctx = (caller ? caller : this)->GetThreadState()->GetContext();
  auto* process = GetMemory()->TranslateVirtual<X_KPROCESS*>(process_address);
  const auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(
      *GetMemory(), ctx, &process->thread_list_spinlock, change_irql);
  kernel::RemoveEntryList(&thread->process_threads, GetMemory());
  process->thread_count = process->thread_count - 1;
  kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &process->thread_list_spinlock, old_irql,
                                        change_irql);
}

void KernelThread::Exit(int exit_code) {
  REX_ASSERT(KernelThread::GetCallingThread() == this);
  throw rex::ThreadStop{exit_code};
}

void KernelThread::Finish(int exit_code) {
  thread_state_->ClearStop();
  if (!host_fn_ && !StateOf(*runtime_).IsTerminatingTitle()) {
    try {
      CallThreadNotifyRoutines(false);
    } catch (const rex::ThreadStop& stop) {
      REXSYS_DEBUG("KernelThread{:08X} exited with code {} inside a thread notify routine",
                   GetHandle(), stop.exit_code);
    }
  }
  exiting_ = true;

  GetGuestObject<X_KTHREAD>()->terminated = 1;
  RundownAPCs();
  WriteExitState(static_cast<u32>(exit_code));
  FreeStack();
  if (main_fiber_) {
    main_fiber_->Destroy();
    main_fiber_ = nullptr;
  }
  ReleaseSelf();
}

void KernelThread::ReleaseSelf() {
  if (released_.exchange(true)) {
    return;
  }
  ThreadRegistry* registry = registry_;
  running_ = false;
  if (IsInThread(this)) {
    current_thread_tls = nullptr;
  }
  registry->MarkExited(*this);
  ReleaseReference();
}

void KernelThread::ReleaseReference() {
  log::NoThrow(log::system, [this] {
    IObjectTable& table = runtime_->GetObjectTable();
    ObjectPin self = table.GetByAddress(guest_object_ptr_);
    if (!self) {
      REXSYS_WARN("KernelThread{:08X}: its object has left the table", handle_);
      return;
    }
    if (auto released = table.Dereference(self); !released) {
      REXSYS_WARN("KernelThread{:08X}: the self reference was not released, {}", handle_,
                  released.error().message);
    }
  });
}

int KernelThread::Execute() {
  REXSYS_DEV("Execute thid {} (handle={:08X}, '{}', native={:08X})", thread_id_, GetHandle(),
             thread_name_, thread_->GetSystemID());

  thread_state_->GetContext()->fpscr.InitHost();
  CheckStop();

  if (host_fn_) {
    return host_fn_();
  }

  DeliverAPCs();
  CallThreadNotifyRoutines(true);

  u32 address;
  std::vector<u64> args;
  bool want_exit_code;
  int exit_code = 0;

  if (creation_params_.xapi_thread_startup) {
    address = creation_params_.xapi_thread_startup;
    args.push_back(creation_params_.start_address);
    args.push_back(creation_params_.start_context);
    want_exit_code = false;
  } else {
    address = creation_params_.start_address;
    args.push_back(creation_params_.start_context);
    want_exit_code = true;
  }

  TitleRuntime* runtime = runtime_;
  if (!StateOf(*runtime).GetFunctionDispatcher()) {
    REXSYS_ERROR("KernelThread::Execute - Runtime not initialized");
    return 0;
  }

  auto* dispatcher = StateOf(*runtime).GetFunctionDispatcher();
  auto* memory = StateOf(*runtime).GetMemory();
  PPCFunc* func = dispatcher->GetFunction(address);
  if (!func) {
    REXSYS_ERROR("KernelThread::Execute - No function registered at {:08X}", address);
    return 0;
  }

  auto* ctx = thread_state_->GetContext();
  u8* base = memory->GetVirtualMembase();

  if (!args.empty())
    ctx->r3.u64 = args[0];
  if (args.size() > 1)
    ctx->r4.u64 = args[1];
  if (args.size() > 2)
    ctx->r5.u64 = args[2];
  if (args.size() > 3)
    ctx->r6.u64 = args[3];
  if (args.size() > 4)
    ctx->r7.u64 = args[4];
  if (args.size() > 5)
    ctx->r8.u64 = args[5];
  if (args.size() > 6)
    ctx->r9.u64 = args[6];
  if (args.size() > 7)
    ctx->r10.u64 = args[7];

  // Windows requires the thread to be a fiber before CreateFiber.
  main_fiber_ = rex::platform::Fiber::ConvertCurrentThread();

  REXSYS_DEV("KernelThread::Execute - Calling function at {:08X}", address);
  const u32 root_r1 = rex::ppc::OpenCallFrame(*ctx, base, 0);
  func(*ctx, base);
  ctx->r1.u64 = root_r1;

  exit_code = static_cast<int>(ctx->r3.u32);

  return want_exit_code ? exit_code : 0;
}

void KernelThread::EnterCriticalRegion() {
  GetGuestObject<X_KTHREAD>()->apc_disable_count--;
}

void KernelThread::LeaveCriticalRegion() {
  auto kthread = GetGuestObject<X_KTHREAD>();
  auto apc_disable_count = ++kthread->apc_disable_count;
  if (apc_disable_count == 0) {
    // APCs are not delivered here because they could fire in the wrong context.
  }
}

void KernelThread::EnqueueAPC(u32 normal_routine, u32 normal_context, u32 arg1, u32 arg2) {
  u32 apc_ptr = GetMemory()->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    REXSYS_WARN("EnqueueAPC: allocation failed (thid={}, normal={:08X})", thread_id_,
                normal_routine);
    return;
  }
  auto apc = GetMemory()->TranslateVirtual<XAPC*>(apc_ptr);
  kernel::xboxkrnl::KeInitializeApc(apc, GetGuestObject(), XAPC::kDummyKernelRoutine,
                                    XAPC::kDummyRundownRoutine, normal_routine,
                                    1 /* user APC mode */, normal_context);

  kernel::ThreadState* caller = kernel::ThreadState::Get();
  if (!caller) {
    REXSYS_FATAL("EnqueueAPC called on a host thread with no bound thread state");
  }
  PPCContext* queue_ctx = caller->GetContext();

  if (!kernel::xboxkrnl::KeInsertQueueApc(*GetMemory(), apc, arg1, arg2, 0, queue_ctx)) {
    GetMemory()->SystemHeapFree(apc_ptr);
    REXSYS_ERROR(
        "EnqueueAPC: queue rejected (thid={}, normal={:08X}, ctx={:08X}, arg1={:08X}, "
        "arg2={:08X})",
        thread_id_, normal_routine, normal_context, arg1, arg2);
    return;
  }
  // The user callback only wakes the thread. Alertable waits deliver APCs.
  thread_->QueueUserCallback([]() {});
}

bool KernelThread::HasPendingAPCs() {
  auto* ctx = thread_state_->GetContext();
  auto kthread = GetGuestObject<X_KTHREAD>();
  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx, &kthread->apc_lock);
  const bool pending = !kthread->apc_lists[1].empty(GetMemory()) && kthread->apc_disable_count == 0;
  kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &kthread->apc_lock, old_irql);
  return pending;
}

namespace {

struct XAPC_DISPATCH_ARGS {
  be_u32 normal_routine;
  be_u32 normal_context;
  be_u32 arg1;
  be_u32 arg2;
};
REX_STATIC_ASSERT_SIZE(XAPC_DISPATCH_ARGS, 0x10);
static_assert(offsetof(XAPC_DISPATCH_ARGS, normal_context) == 4);
static_assert(offsetof(XAPC_DISPATCH_ARGS, arg1) == 8);
static_assert(offsetof(XAPC_DISPATCH_ARGS, arg2) == 12);

}  // namespace

void KernelThread::DeliverAPCs() {
  auto mem = GetMemory();
  auto* ctx = thread_state_->GetContext();
  auto kthread = GetGuestObject<X_KTHREAD>();
  auto* dispatcher = StateOf(*runtime_).GetFunctionDispatcher();

  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx, &kthread->apc_lock);
  auto& user_apc_queue = kthread->apc_lists[1];

  while (!user_apc_queue.empty(mem) && kthread->apc_disable_count == 0) {
    REX_PERF_INC("apc_queue_depth");
    XAPC* apc = user_apc_queue.HeadObject(mem);
    u32 apc_ptr = mem->HostToGuestVirtual(apc);

    kernel::RemoveEntryList(&apc->list_entry, mem);
    apc->enqueued = 0;

    kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &kthread->apc_lock, old_irql);

    auto* args = mem->TranslateVirtual<XAPC_DISPATCH_ARGS*>(scratch_address_);
    args->normal_routine = apc->normal_routine;
    args->normal_context = apc->normal_context;
    args->arg1 = apc->arg1;
    args->arg2 = apc->arg2;

    if (apc->kernel_routine != XAPC::kDummyKernelRoutine) {
      if (dispatcher->GetFunction(apc->kernel_routine)) {
        u64 kernel_args[] = {apc_ptr, scratch_address_ + 0, scratch_address_ + 4,
                             scratch_address_ + 8, scratch_address_ + 12};
        dispatcher->ExecuteTrap(thread_state_.get(), apc->kernel_routine, kernel_args,
                                rex::CountOf(kernel_args));
      } else {
        REXSYS_ERROR("DeliverAPCs: kernel_routine {:08X} not found",
                     static_cast<u32>(apc->kernel_routine));
      }
    } else {
      mem->SystemHeapFree(apc_ptr);
    }

    u32 normal_routine = args->normal_routine;
    u32 normal_context = args->normal_context;
    u32 arg1 = args->arg1;
    u32 arg2 = args->arg2;

    if (normal_routine) {
      if (dispatcher->GetFunction(normal_routine)) {
        u64 normal_args[] = {normal_context, arg1, arg2};
        dispatcher->ExecuteTrap(thread_state_.get(), normal_routine, normal_args,
                                rex::CountOf(normal_args));
      } else {
        REXSYS_ERROR("DeliverAPCs: normal_routine {:08X} not found", normal_routine);
      }
    }

    REXSYS_DEBUG("Completed delivery of APC to {:08X} ({:08X}, {:08X}, {:08X})", normal_routine,
                 normal_context, arg1, arg2);

    old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx, &kthread->apc_lock);
  }

  kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &kthread->apc_lock, old_irql);
}

void KernelThread::RundownAPCs() {
  REX_ASSERT(KernelThread::GetCallingThread() == this);
  auto mem = GetMemory();
  auto kthread = GetGuestObject<X_KTHREAD>();
  auto* ctx = thread_state_->GetContext();

  for (int mode = 1; mode >= 0; --mode) {
    auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx, &kthread->apc_lock);
    auto& apc_queue = kthread->apc_lists[mode];

    while (!apc_queue.empty(mem)) {
      XAPC* apc = apc_queue.HeadObject(mem);
      u32 apc_ptr = mem->HostToGuestVirtual(apc);
      bool needs_freeing = apc->kernel_routine == XAPC::kDummyKernelRoutine;

      kernel::RemoveEntryList(&apc->list_entry, mem);
      apc->enqueued = 0;

      kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &kthread->apc_lock, old_irql);

      if (apc->rundown_routine == XAPC::kDummyRundownRoutine) {
      } else if (apc->rundown_routine) {
        auto* dispatcher = StateOf(*runtime_).GetFunctionDispatcher();
        if (dispatcher->GetFunction(apc->rundown_routine)) {
          u64 rundown_args[] = {apc_ptr};
          dispatcher->Execute(thread_state_.get(), apc->rundown_routine, rundown_args,
                              rex::CountOf(rundown_args));
        } else {
          REXSYS_WARN("RundownAPCs: rundown_routine {:08X} not found",
                      static_cast<u32>(apc->rundown_routine));
        }
      }

      if (needs_freeing) {
        mem->SystemHeapFree(apc_ptr);
      }

      old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*GetMemory(), ctx, &kthread->apc_lock);
    }
    kernel::xboxkrnl::KeKfReleaseSpinLock(*GetMemory(), ctx, &kthread->apc_lock, old_irql);
  }
}

void KernelThread::CallThreadNotifyRoutines(bool create) {
  auto* dispatcher = StateOf(*runtime_).GetFunctionDispatcher();
  for (const u32 notification : registry_->GetThreadNotifications()) {
    const u32 routine =
        GetMemory()->TranslateVirtual<X_THREAD_NOTIFICATION*>(notification)->notify_routine;
    u64 args[] = {create ? 1u : 0u};
    dispatcher->ExecuteTrap(thread_state_.get(), routine, args, rex::CountOf(args));
  }
}

i32 KernelThread::GetHostPriority() {
  return thread_->GetPriority();
}

void KernelThread::SetPriority(i32 increment) {
  priority_ = increment;

  auto kthread = GetGuestObject<X_KTHREAD>();
  kthread->priority = static_cast<u8>(std::clamp(increment, 0, 31));

  i32 target_priority = 0;
  if (increment > 0x22) {
    target_priority = rex::platform::ThreadPriority::kHighest;
  } else if (increment > 0x11) {
    target_priority = rex::platform::ThreadPriority::kAboveNormal;
  } else if (increment < -0x22) {
    target_priority = rex::platform::ThreadPriority::kLowest;
  } else if (increment < -0x11) {
    target_priority = rex::platform::ThreadPriority::kBelowNormal;
  } else {
    target_priority = rex::platform::ThreadPriority::kNormal;
  }
  if (!GetRuntimeSettings().ignore_thread_priorities.Get()) {
    thread_->SetPriority(target_priority);
  }
}

bool KernelThread::SetAffinity(u32 affinity) {
  if (!affinity || (affinity & ~0x3Fu)) {
    return false;
  }
  SetActiveCPU(static_cast<u8>(std::bit_width(affinity) - 1));
  return true;
}

u8 KernelThread::GetActiveCPU() const {
  // pcr_address_ may be unset, so read the guest KTHREAD.
  if (IsGuestThread()) {
    auto* kthread = GetMemory()->TranslateVirtual<const X_KTHREAD*>(GetGuestObject());
    return kthread->current_cpu;
  }
  if (!pcr_address_) {
    return 0;
  }
  const X_KPCR& pcr = *GetMemory()->TranslateVirtual<const X_KPCR*>(pcr_address_);
  return pcr.prcb_data.current_cpu;
}

void KernelThread::SetActiveCPU(u8 cpu_index) {
  // Called during thread creation, so it must not skip when the CPU is unchanged.

  REX_ASSERT(cpu_index < 6);

  if (IsGuestThread()) {
    X_KTHREAD& thread_object = *GetMemory()->TranslateVirtual<X_KTHREAD*>(GetGuestObject());
    thread_object.current_cpu = cpu_index;
  }

  if (pcr_address_) {
    X_KPCR& pcr = *GetMemory()->TranslateVirtual<X_KPCR*>(pcr_address_);
    pcr.prcb_data.current_cpu = cpu_index;
  }

  if (rex::platform::GetLogicalProcessorCount() >= 6) {
    if (!GetRuntimeSettings().ignore_thread_affinities.Get()) {
      thread_->SetAffinityMask(static_cast<u64>(1) << cpu_index);
    }
  } else {
    REXSYS_WARN("Too few processor cores - scheduling will be wonky");
  }
}

bool KernelThread::GetTLSValue(u32 slot, u32* value_out) const {
  if (slot >= tls_slot_count_.load(std::memory_order_acquire)) {
    return false;
  }

  auto mem = GetMemory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = memory::load<be_u32>(mem);
  return true;
}

bool KernelThread::SetTLSValue(u32 slot, u32 value) {
  if (slot >= tls_slot_count_.load(std::memory_order_acquire)) {
    return false;
  }

  auto mem = GetMemory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  memory::store<be_u32>(mem, value);
  return true;
}

u32 KernelThread::GetSuspendCount() {
  std::lock_guard lock(suspend_wait_->mutex);
  return GetGuestObject<X_KTHREAD>()->suspend_count;
}

X_STATUS KernelThread::Resume(u32* out_suspend_count) {
  auto guest_thread = GetGuestObject<X_KTHREAD>();
  std::lock_guard lock(suspend_wait_->mutex);
  const u8 previous = guest_thread->suspend_count;
  if (out_suspend_count) {
    *out_suspend_count = previous;
  }
  if (previous == 0) {
    return X_STATUS_SUCCESS;
  }
  guest_thread->suspend_count = static_cast<u8>(previous - 1);
  if (previous > 1) {
    return X_STATUS_SUCCESS;
  }
  if (suspend_deferred_.exchange(false, std::memory_order_acq_rel)) {
    suspend_wait_->cv.notify_all();
    return X_STATUS_SUCCESS;
  }
  return thread_->Resume() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS KernelThread::Suspend(u32* out_suspend_count) {
  if (IsInThread(this)) {
    const u32 previous = SelfSuspend();
    if (out_suspend_count) {
      *out_suspend_count = previous;
    }
    return X_STATUS_SUCCESS;
  }
  auto guest_thread = GetGuestObject<X_KTHREAD>();
  std::lock_guard lock(suspend_wait_->mutex);
  const u8 previous = guest_thread->suspend_count;
  if (out_suspend_count) {
    *out_suspend_count = previous;
  }
  guest_thread->suspend_count = static_cast<u8>(previous + 1);
  if (previous > 0 || guest_thread->suspend_count == 0) {
    return X_STATUS_SUCCESS;
  }
  const auto in_recompiled_code = [](u64 pc, void*) { return rex::ppc::IsRecompiledCode(pc); };
  if (thread_->SuspendIf(in_recompiled_code, nullptr)) {
    return X_STATUS_SUCCESS;
  }
  suspend_deferred_.store(true, std::memory_order_release);
  thread_->QueueUserCallback([] {});
  return X_STATUS_SUCCESS;
}

u32 KernelThread::SelfSuspend() {
  CheckStop();
  auto guest_thread = GetGuestObject<X_KTHREAD>();
  u32 previous = 0;
  {
    std::unique_lock lock(suspend_wait_->mutex);
    previous = guest_thread->suspend_count;
    guest_thread->suspend_count = static_cast<u8>(previous + 1);
    if (previous == 0 && guest_thread->suspend_count != 0) {
      suspend_deferred_.store(true, std::memory_order_release);
    }
    suspend_wait_->cv.wait(lock,
                           [this] { return !suspend_deferred_.load(std::memory_order_acquire); });
  }
  CheckStop();
  return previous;
}

void KernelThread::TakeDeferredSuspend() {
  std::unique_lock lock(suspend_wait_->mutex);
  suspend_wait_->cv.wait(lock,
                         [this] { return !suspend_deferred_.load(std::memory_order_acquire); });
}

namespace {

std::atomic<DelayObserver> g_delay_observer{nullptr};

void ObserveDelay(std::chrono::microseconds duration, bool alertable) {
  if (const auto observer = g_delay_observer.load(std::memory_order_acquire)) {
    observer(duration, alertable);
  }
}

}  // namespace

void SetDelayObserverForTest(DelayObserver observer) {
  g_delay_observer.store(observer, std::memory_order_release);
}

X_STATUS KernelThread::Delay([[maybe_unused]] u32 processor_mode, u32 alertable,
                             u64 interval) const {
  const auto duration = DelayFromTicks(static_cast<i64>(interval));
  CheckStop();
  WriteKernelTime();
  if (alertable) {
    ObserveDelay(duration, true);
    auto result = rex::platform::AlertableSleep(duration);
    CheckStop();
    switch (result) {
      default:
      case rex::platform::SleepResult::kSuccess:
        return X_STATUS_SUCCESS;
      case rex::platform::SleepResult::kAlerted:
        return X_STATUS_USER_APC;
    }
  } else {
    if (duration == std::chrono::microseconds::zero()) {
      if (priority_ <= rex::platform::ThreadPriority::kBelowNormal) {
        rex::platform::SleepFor(std::chrono::microseconds(100));
      } else {
        rex::platform::MaybeYield();
      }
    } else {
      rex::platform::MaybeYield();
      ObserveDelay(duration, false);
      SleepFor(duration);
    }
    CheckStop();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel
