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
#include <cstring>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/literals.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/platform/exceptions.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/process.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/runtime/xthread.h>
#include <rex/platform/thread.h>
#include <rex/vec128.h>
#include "runtime/cvar_table.h"
#include "runtime/thread_registry.h"
#include "runtime/guest_stack.h"

namespace rex::runtime {

namespace {

thread_local std::unique_ptr<rex::platform::Thread> t_exiting_thread;

}  // namespace

ThreadRecord::ThreadRecord(Runtime& runtime, uint32_t stack_size, uint32_t xapi_thread_startup,
                           uint32_t start_address, uint32_t start_context, uint32_t creation_flags,
                           bool guest_thread, bool main_thread, uint32_t guest_process)
    : guest_thread_(guest_thread),
      main_thread_(main_thread),
      apc_lock_old_irql_(0),
      runtime_(&runtime) {
  creation_params_.stack_size = stack_size;
  creation_params_.xapi_thread_startup = xapi_thread_startup;
  creation_params_.start_address = start_address;
  creation_params_.start_context = start_context;

  // top 8 bits = processor ID (or 0 for default)
  // bit 0 = 1 to create suspended
  creation_params_.creation_flags = creation_flags;
  creation_params_.guest_process = guest_process;

  exit_gate_ = rex::platform::Event::CreateManualResetEvent(false);
}

ThreadRecord::ThreadRecord(Runtime& runtime, uint32_t stack_size, uint32_t creation_flags,
                           std::function<int()> host_fn)
    : ThreadRecord(runtime, stack_size, 0, 0, 0, creation_flags, false) {
  host_fn_ = std::move(host_fn);
}

ThreadRecord::~ThreadRecord() {
  if (main_fiber_) {
    main_fiber_->Destroy();
    main_fiber_ = nullptr;
  }

  if (thread_ && thread_.get() == rex::platform::Thread::GetCurrentThread()) {
    t_exiting_thread = std::move(thread_);
  }
  thread_.reset();

  runtime_->memory()->SystemHeapFree(scratch_address_);
  FreeStack();
  runtime_->memory()->SystemHeapFree(pcr_address_);
  FreeGuestBlock(memory(), guest_object_ptr_);
}

runtime::Memory* ThreadRecord::memory() const {
  return runtime_->memory();
}

u32 ThreadRecord::ProcessAddress() const {
  return creation_params_.guest_process ? creation_params_.guest_process
                                        : runtime_->process_info_block_address();
}

u8 ThreadRecord::ProcessorFromMask(u8 mask) {
  assert_false(mask & 0xC0);
  const u8 processor = static_cast<u8>(7 - std::countl_zero(mask));
  assert_true((1u << processor) == mask);
  return processor;
}

thread_local ThreadRecord* current_thread_tls_ = nullptr;

namespace {

ThreadRecord* GetBoundCurrentThread() {
  return current_thread_tls_;
}

}  // namespace

bool ThreadRecord::IsInThread() {
  return GetBoundCurrentThread() != nullptr;
}

bool ThreadRecord::IsInThread(const ThreadRecord* other) {
  return GetBoundCurrentThread() == other;
}

ThreadRecord* ThreadRecord::GetCurrentThread() {
  ThreadRecord* thread = GetBoundCurrentThread();
  if (!thread) {
    assert_always("Attempting to use kernel stuff from a non-kernel thread");
  }
  return thread;
}

void ThreadRecord::CheckTitleTermination() {
  ThreadRecord* self = GetBoundCurrentThread();
  if (self && !self->exiting_ && self->is_guest_thread() && self->is_running() &&
      self->runtime().is_terminating_title()) {
    // Unwind cleanly at this safe point. Does not return.
    self->Exit(0);
  }
}

uint32_t ThreadRecord::GetCurrentThreadHandle() {
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();
  return thread->handle();
}

uint32_t ThreadRecord::GetCurrentThreadId() {
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();
  return thread->guest_object<X_KTHREAD>()->thread_id;
}

uint32_t ThreadRecord::GetLastError() {
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();
  return thread->last_error();
}

void ThreadRecord::SetLastError(uint32_t error_code) {
  ThreadRecord* thread = ThreadRecord::GetCurrentThread();
  thread->set_last_error(error_code);
}

uint32_t ThreadRecord::last_error() {
  return guest_object<X_KTHREAD>()->last_error;
}

void ThreadRecord::set_last_error(uint32_t error_code) {
  guest_object<X_KTHREAD>()->last_error = error_code;
}

void ThreadRecord::set_name(const std::string_view name) {
  std::lock_guard<std::mutex> lock(thread_lock_);
  thread_name_ = fmt::format("{} ({:08X})", name, handle());

  if (thread_) {
    // May be getting set before the thread is created.
    // One the thread is ready it will handle it.
    thread_->set_name(thread_name_);
  }
}

void ThreadRecord::InitializeGuestObject() {
  auto guest_thread = guest_object<X_KTHREAD>();
  uint32_t guest_ptr = guest_object();

  guest_thread->header.type = 6;  // ThreadObject
  guest_thread->suspend_count = (creation_params_.creation_flags & X_CREATE_SUSPENDED) ? 1 : 0;

  // Self-referencing pointers for wait timeout timer/block
  guest_thread->unk_10 = guest_ptr + 0x010;
  guest_thread->unk_14 = guest_ptr + 0x010;

  guest_thread->wait_timeout_block.wait_list_entry.flink_ptr = guest_ptr + 0x20;
  guest_thread->wait_timeout_block.wait_list_entry.blink_ptr = guest_ptr + 0x20;
  guest_thread->wait_timeout_block.thread = guest_ptr;
  guest_thread->wait_timeout_block.object = guest_ptr + 0x18;
  guest_thread->wait_timeout_block.wait_result_xstatus = 0x0100;
  guest_thread->wait_timeout_block.wait_type = 0x0201;

  guest_thread->stack_base = stack_base_;
  guest_thread->stack_limit = stack_limit_;
  guest_thread->stack_kernel = stack_base_ - 240;
  guest_thread->tls_address = tls_dynamic_address_;
  guest_thread->thread_state = 0;

  // Initialize APC lists (kernel + user mode)
  guest_thread->apc_lists[0].Initialize(memory());
  guest_thread->apc_lists[1].Initialize(memory());

  uint32_t process_ptr = ProcessAddress();
  guest_thread->process = process_ptr;
  guest_thread->may_queue_apcs = 1;

  // Set PRCB pointers (derived from this thread's PCR).
  uint32_t kpcrb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  guest_thread->a_prcb_ptr = kpcrb;
  guest_thread->another_prcb_ptr = kpcrb;

  // PPCContext for spinlock helpers (valid before thread runs; r13 set at construction).
  auto* ctx = thread_state_->context();

  // Set per-thread process type and link into process thread list.
  if (process_ptr) {
    auto target_process = memory()->TranslateVirtual<X_KPROCESS*>(process_ptr);
    guest_thread->process_type = target_process->process_type;
    guest_thread->process_type_dup = target_process->process_type;

    auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), ctx,
                                                          &target_process->thread_list_spinlock);
    util::XeInsertTailList(&target_process->thread_list, &guest_thread->process_threads, memory());
    target_process->thread_count = target_process->thread_count + 1;
    kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, &target_process->thread_list_spinlock,
                                          old_irql);
  } else {
    guest_thread->process_type = X_PROCTYPE_USER;
    guest_thread->process_type_dup = X_PROCTYPE_USER;
  }

  guest_thread->msr_mask = 0xFDFFD7FF;
  // current_cpu is expected to be initialized externally via SetActiveCPU.
  guest_thread->stack_alloc_base = stack_base_;
  guest_thread->create_time = Clock::QueryGuestSystemTime();

  // Initialize timer_list as self-referencing
  guest_thread->timer_list.flink_ptr = guest_ptr + offsetof(X_KTHREAD, timer_list);
  guest_thread->timer_list.blink_ptr = guest_ptr + offsetof(X_KTHREAD, timer_list);

  guest_thread->thread_id = thread_id_;
  guest_thread->start_address = creation_params_.start_address;

  // Initialize unk_154 list as self-referencing
  guest_thread->unk_154.flink_ptr = guest_ptr + offsetof(X_KTHREAD, unk_154);
  guest_thread->unk_154.blink_ptr = guest_ptr + offsetof(X_KTHREAD, unk_154);

  guest_thread->last_error = 0;
  guest_thread->creation_flags = creation_params_.creation_flags;
  guest_thread->unk_17C = 1;
}

void ThreadRecord::FreeStack() {
  if (!stack_alloc_base_) {
    return;
  }
  const u32 globals_address = runtime_->GetKernelGuestGlobals();
  ThreadRecord* caller = GetBoundCurrentThread();
  ThreadRecord* owner = caller ? caller : this;
  auto* lock = globals_address
                   ? &memory()->TranslateVirtual<KernelGuestGlobals*>(globals_address)->tls_lock
                   : nullptr;
  if (lock && owner->thread_state_ && lock->prcb_of_owner != pcr_address_) {
    const bool change_irql = caller != nullptr;
    auto* ctx = owner->thread_state_->context();
    const auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), ctx, lock, change_irql);
    tls_slot_count_.store(0, std::memory_order_release);
    kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, lock, old_irql, change_irql);
  } else {
    tls_slot_count_.store(0, std::memory_order_release);
  }
  FreeGuestStack(*memory(), stack_alloc_base_);
  stack_alloc_base_ = 0;
  stack_base_ = 0;
  stack_limit_ = 0;
}

X_STATUS ThreadRecord::Create() {
  const u32 process_address = ProcessAddress();
  const auto* process =
      process_address ? memory()->TranslateVirtual<const X_KPROCESS*>(process_address) : nullptr;
  const u32 guest_default = process ? static_cast<u32>(process->kernel_stack_size) : 0;
  const u32 data_size =
      process ? rex::round_up(static_cast<u32>(process->tls_data_size), 4u, false) : 0;
  const u32 slot_count =
      process ? static_cast<u32>(process->tls_slot_size) / 4 : ResolveTLSSlotCount(0);
  const u32 tls_size = data_size + slot_count * 4;
  const u32 stack_size =
      ResolveGuestStackSize(creation_params_.stack_size, guest_default, kMinimumThreadStackSize);
  Result<GuestStack> stack = AllocateGuestStack(*memory(), stack_size, tls_size, guest_thread_);
  if (!stack) {
    return X_STATUS_NO_MEMORY;
  }
  stack_alloc_base_ = stack->alloc_base;
  stack_limit_ = stack->limit;
  stack_base_ = stack->base;

  scratch_size_ = 4 * 16;
  scratch_address_ = memory()->SystemHeapAlloc(scratch_size_);

  tls_static_address_ = stack_base_;
  tls_dynamic_address_ = stack_base_ + data_size;
  memory()->Fill(tls_static_address_, tls_size, 0);
  if (process && process->tls_raw_data_size) {
    memory()->Copy(tls_static_address_, process->tls_static_data_address,
                   std::min(static_cast<u32>(process->tls_raw_data_size), data_size));
  }
  tls_slot_count_.store(slot_count, std::memory_order_release);

  // Allocate thread state block from heap.
  // https://web.archive.org/web/20170704035330/https://www.microsoft.com/msj/archive/S2CE.aspx
  // This is set as r13 for user code and some special inlined Win32 calls
  // (like GetLastError/etc) will poke it directly.
  // We try to use it as our primary store of data just to keep things all
  // consistent.
  // 0x000: pointer to tls data
  // 0x100: pointer to TEB(?)
  // 0x10C: Current CPU(?)
  // 0x150: if >0 then error states don't get set (DPC active bool?)
  // TEB:
  // 0x14C: thread id
  // 0x160: last error
  // So, at offset 0x100 we have a 4b pointer to offset 200, then have the
  // structure.
  pcr_address_ = memory()->SystemHeapAlloc(0x2D8);
  if (!pcr_address_) {
    REXLOG_WARN(system, "Unable to allocate thread state block");
    return X_STATUS_NO_MEMORY;
  }

  // Create thread state - needed for interrupt callbacks and kernel exports
  thread_state_ = std::make_unique<runtime::ThreadState>(thread_id_, stack_base_, pcr_address_,
                                                         memory(), runtime_);

  REXLOG_DEV(system, "XThread{:08X} ({:X}) Stack: {:08X}-{:08X}", handle(), thread_id_,
             stack_limit_, stack_base_);

  const u8 processor_mask = static_cast<u8>(creation_params_.creation_flags >> 24);
  u8 cpu_index = 0;
  if (processor_mask) {
    cpu_index = ProcessorFromMask(processor_mask);
  } else if (ThreadRecord* creator = GetBoundCurrentThread();
             creator && creator->is_guest_thread()) {
    cpu_index = creator->active_cpu();
  }

  // Initialize the KTHREAD object.
  InitializeGuestObject();

  X_KPCR* pcr = memory()->TranslateVirtual<X_KPCR*>(pcr_address_);

  pcr->tls_ptr = tls_static_address_;
  pcr->pcr_ptr = pcr_address_;
  pcr->prcb_data.current_thread = guest_object();
  pcr->prcb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  pcr->host_stash = reinterpret_cast<uint64_t>(thread_state_->context());
  pcr->current_irql = 0;

  pcr->stack_base_ptr = stack_base_;
  pcr->stack_end_ptr = stack_limit_;

  pcr->prcb_data.dpc_active = 0;

  // Always retain when starting - the thread owns itself until exited.
  (void)runtime_->object_table().Retain(handle_);

  rex::platform::Thread::CreationParameters params;
  params.stack_size = 16_MiB;  // Allocate a big host stack.
  params.create_suspended = true;
  thread_ = rex::platform::Thread::Create(params, [this]() {
    rex::initialize_seh_thread();
    runtime::ThreadState::Bind(thread_state_.get());

    // Set thread ID override. This is used by logging.
    rex::platform::set_current_thread_id(handle());

    // Set name immediately, if we have one.
    thread_->set_name(thread_name_);

    REX_PROFILE_THREAD(thread_name_.c_str());
    REX_PERF_INC(kActiveThreads);

    // Execute user code.
    current_thread_tls_ = this;
    started_ = true;
    registry_->NoteStarted();
    running_ = true;
    Execute();
    running_ = false;
    current_thread_tls_ = nullptr;

    REX_PERF_ADD(kActiveThreads, -1);

    // Release the self-reference to the thread.
    ReleaseSelf();
  });

  if (!thread_) {
    // TODO(benvanik): translate error?
    REX_FAIL(system, X_STATUS_NO_MEMORY, "CreateThread failed");
  }

  // Set the thread name based on host ID (for easier debugging).
  if (thread_name_.empty()) {
    set_name(fmt::format("XThread{:04X}", thread_->system_id()));
  }

  if (creation_params_.creation_flags & 0x60) {
    thread_->set_priority(creation_params_.creation_flags & 0x20 ? 1 : 0);
  }

  // Assign the newly created thread to the logical processor, and also set up
  // the current CPU in KPCR and KTHREAD.
  SetActiveCPU(cpu_index);

  if ((creation_params_.creation_flags & X_CREATE_SUSPENDED) == 0) {
    // Start the thread now that we're all setup.
    thread_->Resume();
  }

  return X_STATUS_SUCCESS;
}

void ThreadRecord::NoteKernelTime() {
  ThreadRecord* self = GetBoundCurrentThread();
  if (!self || !self->guest_object_ptr_) {
    return;
  }
  self->guest_object<X_KTHREAD>()->kernel_time =
      static_cast<u32>(rex::platform::QueryCurrentThreadCPUTime().count());
}

void ThreadRecord::WriteExitState(u32 exit_code) {
  auto* thread = guest_object<X_KTHREAD>();
  thread->terminated = 1;
  thread->thread_state = KTHREAD_STATE_TERMINATED;
  if (IsInThread(this)) {
    NoteKernelTime();
  }
  thread->exit_time = Clock::QueryGuestSystemTime();
  thread->exit_status = exit_code;
  thread->header.signal_state = 1;
  const u32 process_address = thread->process;
  if (!process_address) {
    return;
  }
  ThreadRecord* caller = GetBoundCurrentThread();
  const bool change_irql = caller != nullptr;
  auto* ctx = (caller ? caller : this)->thread_state()->context();
  auto* process = memory()->TranslateVirtual<X_KPROCESS*>(process_address);
  const auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(
      *memory(), ctx, &process->thread_list_spinlock, change_irql);
  util::XeRemoveEntryList(&thread->process_threads, memory());
  process->thread_count = process->thread_count - 1;
  kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, &process->thread_list_spinlock, old_irql,
                                        change_irql);
}

X_STATUS ThreadRecord::Exit(int exit_code) {
  assert_true(ThreadRecord::GetCurrentThread() == this);

  if (exit_claimed_.exchange(true)) {
    rex::platform::Wait(exit_gate_.get(), false, std::chrono::milliseconds::max());
  }
  exiting_ = true;

  if (!host_fn_ && !runtime_->is_terminating_title()) {
    CallThreadNotifyRoutines(false);
  }

  guest_object<X_KTHREAD>()->terminated = 1;
  RundownAPCs();
  WriteExitState(static_cast<u32>(exit_code));
  FreeStack();
  ReleaseSelf();
  rex::platform::Thread::Exit(exit_code);
  return X_STATUS_SUCCESS;
}

X_STATUS ThreadRecord::Terminate(int exit_code) {
  if (exit_claimed_.exchange(true)) {
    return X_STATUS_SUCCESS;
  }

  WriteExitState(static_cast<u32>(exit_code));
  if (ThreadRecord::IsInThread(this)) {
    FreeStack();
    ReleaseSelf();
    rex::platform::Thread::Exit(exit_code);
  } else {
    thread_->Terminate(exit_code);
    FreeStack();
    ReleaseSelf();
  }

  return X_STATUS_SUCCESS;
}

void ThreadRecord::ReleaseSelf() {
  if (released_.exchange(true)) {
    return;
  }
  const X_HANDLE handle = handle_;
  ThreadRegistry* registry = registry_;
  const bool started = started_;
  running_ = false;
  if (IsInThread(this)) {
    current_thread_tls_ = nullptr;
  }
  registry->NoteExited(*this);
  (void)runtime_->object_table().Close(handle);
  if (started) {
    registry->NoteStopped();
  }
}

void ThreadRecord::Execute() {
  REXLOG_DEV(system, "Execute thid {} (handle={:08X}, '{}', native={:08X})", thread_id_, handle(),
             thread_name_, thread_->system_id());

  thread_state_->context()->fpscr.InitHost();

  if (host_fn_) {
    Exit(host_fn_());
    return;
  }

  // Dispatch any APCs that were queued before the thread was created first.
  DeliverAPCs();
  CallThreadNotifyRoutines(true);

  uint32_t address;
  std::vector<uint64_t> args;
  bool want_exit_code;
  int exit_code = 0;

  // If a XapiThreadStartup value is present, we use that as a trampoline.
  // Otherwise, we are a raw thread.
  if (creation_params_.xapi_thread_startup) {
    address = creation_params_.xapi_thread_startup;
    args.push_back(creation_params_.start_address);
    args.push_back(creation_params_.start_context);
    want_exit_code = false;
  } else {
    // Run user code.
    address = creation_params_.start_address;
    args.push_back(creation_params_.start_context);
    want_exit_code = true;
  }

  Runtime* runtime = runtime_;
  if (!runtime->function_dispatcher()) {
    REXLOG_ERROR(system, "ThreadRecord::Execute - Runtime not initialized");
    return;
  }

  auto* dispatcher = runtime->function_dispatcher();
  auto* memory = runtime->memory();
  PPCFunc* func = dispatcher->GetFunction(address);
  if (!func) {
    REXLOG_ERROR(system, "ThreadRecord::Execute - No function registered at {:08X}", address);
    return;
  }

  auto* ctx = thread_state_->context();
  uint8_t* base = memory->virtual_membase();

  // Pass arguments in r3, r4, ... per PPC calling convention
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

  // Convert this host thread to a fiber so SwitchTo works bidirectionally.
  // Required on Windows before any CreateFiber; provides the fallback handle
  // when another fiber switches back to the main execution context.
  main_fiber_ = rex::platform::Fiber::ConvertCurrentThread();

  // Execute the function
  REXLOG_DEV(system, "ThreadRecord::Execute - Calling function at {:08X}", address);
  func(*ctx, base);

  exit_code = static_cast<int>(ctx->r3.u32);

  // If we got here it means the execute completed without an exit being called.
  // Treat the return code as an implicit exit code (if desired).
  Exit(!want_exit_code ? 0 : exit_code);
}

void ThreadRecord::EnterCriticalRegion() {
  guest_object<X_KTHREAD>()->apc_disable_count--;
}

void ThreadRecord::LeaveCriticalRegion() {
  auto kthread = guest_object<X_KTHREAD>();
  auto apc_disable_count = ++kthread->apc_disable_count;
  if (apc_disable_count == 0) {
    // NOTE: intentionally not calling CheckApcs() here.
    // Delivering APCs here can cause them to fire in wrong contexts.
  }
}

void ThreadRecord::LockApc() {
  auto kthread = guest_object<X_KTHREAD>();
  apc_lock_old_irql_ = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), thread_state_->context(),
                                                             &kthread->apc_lock);
}

void ThreadRecord::UnlockApc(bool queue_delivery) {
  auto kthread = guest_object<X_KTHREAD>();
  auto mem = memory();
  bool needs_apc = !kthread->apc_lists[0].empty(mem) || !kthread->apc_lists[1].empty(mem);
  kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), thread_state_->context(), &kthread->apc_lock,
                                        apc_lock_old_irql_);
  if (needs_apc && queue_delivery) {
    // Match Edge/Canary behavior: callback is only a wakeup hint.
    // User APC execution happens on alertable wait return paths.
    thread_->QueueUserCallback([]() {});
  }
}

void ThreadRecord::EnqueueAPC(uint32_t normal_routine, uint32_t normal_context, uint32_t arg1,
                              uint32_t arg2) {
  uint32_t apc_ptr = memory()->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    REXLOG_WARN(system, "EnqueueAPC: allocation failed (thid={}, normal={:08X})", thread_id_,
                normal_routine);
    return;
  }
  auto apc = memory()->TranslateVirtual<XAPC*>(apc_ptr);
  kernel::xboxkrnl::KeInitializeApc(apc, guest_object(), XAPC::kDummyKernelRoutine,
                                    XAPC::kDummyRundownRoutine, normal_routine,
                                    1 /* user apc mode */, normal_context);

  // Important: use the caller PPC context when queuing to another thread.
  // Using the target thread context here can corrupt APC lock/IRQL bookkeeping.
  PPCContext* queue_ctx =
      runtime::ThreadState::Get() ? runtime::current_ppc_context() : thread_state_->context();

  if (!kernel::xboxkrnl::KeInsertQueueApc(*memory(), apc, arg1, arg2, 0, queue_ctx)) {
    memory()->SystemHeapFree(apc_ptr);
    REXLOG_ERROR(system,
                 "EnqueueAPC: queue rejected (thid={}, normal={:08X}, ctx={:08X}, arg1={:08X}, "
                 "arg2={:08X})",
                 thread_id_, normal_routine, normal_context, arg1, arg2);
    return;
  }
  // Match Edge/Canary behavior: callback is only a wakeup hint.
  // APCs are delivered via alertable wait handling.
  thread_->QueueUserCallback([]() {});
}

void ThreadRecord::DeliverAPCs() {
  auto mem = memory();
  auto* ctx = thread_state_->context();
  auto kthread = guest_object<X_KTHREAD>();
  auto* dispatcher = runtime_->function_dispatcher();

  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), ctx, &kthread->apc_lock);
  auto& user_apc_queue = kthread->apc_lists[1];

  while (!user_apc_queue.empty(mem) && kthread->apc_disable_count == 0) {
    REX_PERF_INC(kAPCQueueDepth);
    XAPC* apc = user_apc_queue.HeadObject(mem);
    uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
    bool needs_freeing = apc->kernel_routine != XAPC::kDummyKernelRoutine;

    util::XeRemoveEntryList(&apc->list_entry, mem);
    apc->enqueued = 0;

    kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, &kthread->apc_lock, old_irql);

    uint8_t* scratch_ptr = mem->TranslateVirtual(scratch_address_);
    memory::store_and_swap<uint32_t>(scratch_ptr + 0, apc->normal_routine);
    memory::store_and_swap<uint32_t>(scratch_ptr + 4, apc->normal_context);
    memory::store_and_swap<uint32_t>(scratch_ptr + 8, apc->arg1);
    memory::store_and_swap<uint32_t>(scratch_ptr + 12, apc->arg2);

    if (apc->kernel_routine != XAPC::kDummyKernelRoutine) {
      if (dispatcher->GetFunction(apc->kernel_routine)) {
        uint64_t kernel_args[] = {apc_ptr, scratch_address_ + 0, scratch_address_ + 4,
                                  scratch_address_ + 8, scratch_address_ + 12};
        dispatcher->ExecuteTrap(thread_state_.get(), apc->kernel_routine, kernel_args,
                                rex::countof(kernel_args));
      } else {
        REXLOG_ERROR(system, "DeliverAPCs: kernel_routine {:08X} not found",
                     uint32_t(apc->kernel_routine));
      }
    } else {
      mem->SystemHeapFree(apc_ptr);
      needs_freeing = false;
    }

    uint32_t normal_routine = memory::load_and_swap<uint32_t>(scratch_ptr + 0);
    uint32_t normal_context = memory::load_and_swap<uint32_t>(scratch_ptr + 4);
    uint32_t arg1 = memory::load_and_swap<uint32_t>(scratch_ptr + 8);
    uint32_t arg2 = memory::load_and_swap<uint32_t>(scratch_ptr + 12);

    if (normal_routine) {
      if (dispatcher->GetFunction(normal_routine)) {
        uint64_t normal_args[] = {normal_context, arg1, arg2};
        dispatcher->ExecuteTrap(thread_state_.get(), normal_routine, normal_args,
                                rex::countof(normal_args));
      } else {
        REXLOG_ERROR(system, "DeliverAPCs: normal_routine {:08X} not found", normal_routine);
      }
    }

    REXLOG_DEBUG(system, "Completed delivery of APC to {:08X} ({:08X}, {:08X}, {:08X})",
                 normal_routine, normal_context, arg1, arg2);

    if (needs_freeing) {
      mem->SystemHeapFree(apc_ptr);
    }

    old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), ctx, &kthread->apc_lock);
  }

  kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, &kthread->apc_lock, old_irql);
}

void ThreadRecord::RundownAPCs() {
  assert_true(ThreadRecord::GetCurrentThread() == this);
  auto mem = memory();
  auto kthread = guest_object<X_KTHREAD>();
  auto* ctx = thread_state_->context();

  // Rundown both user (1) and kernel (0) APC lists.
  for (int mode = 1; mode >= 0; --mode) {
    auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), ctx, &kthread->apc_lock);
    auto& apc_queue = kthread->apc_lists[mode];

    while (!apc_queue.empty(mem)) {
      XAPC* apc = apc_queue.HeadObject(mem);
      uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
      bool needs_freeing = apc->kernel_routine == XAPC::kDummyKernelRoutine;

      util::XeRemoveEntryList(&apc->list_entry, mem);
      apc->enqueued = 0;

      kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, &kthread->apc_lock, old_irql);

      if (apc->rundown_routine == XAPC::kDummyRundownRoutine) {
        // No-op.
      } else if (apc->rundown_routine) {
        auto* dispatcher = runtime_->function_dispatcher();
        if (dispatcher->GetFunction(apc->rundown_routine)) {
          uint64_t rundown_args[] = {apc_ptr};
          dispatcher->Execute(thread_state_.get(), apc->rundown_routine, rundown_args,
                              rex::countof(rundown_args));
        } else {
          REXLOG_WARN(system, "RundownAPCs: rundown_routine {:08X} not found",
                      uint32_t(apc->rundown_routine));
        }
      }

      if (needs_freeing) {
        mem->SystemHeapFree(apc_ptr);
      }

      old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*memory(), ctx, &kthread->apc_lock);
    }
    kernel::xboxkrnl::KeKfReleaseSpinLock(*memory(), ctx, &kthread->apc_lock, old_irql);
  }
}

void ThreadRecord::CallThreadNotifyRoutines(bool create) {
  auto* dispatcher = runtime_->function_dispatcher();
  for (const u32 notification : registry_->ThreadNotifications()) {
    const u32 routine =
        memory()->TranslateVirtual<X_THREAD_NOTIFICATION*>(notification)->notify_routine;
    u64 args[] = {create ? 1u : 0u};
    dispatcher->ExecuteTrap(thread_state_.get(), routine, args, rex::countof(args));
  }
}

int32_t ThreadRecord::QueryPriority() {
  return thread_->priority();
}

void ThreadRecord::SetPriority(int32_t increment) {
  priority_ = increment;

  // Write priority to guest X_KTHREAD struct.
  auto kthread = guest_object<X_KTHREAD>();
  kthread->priority = static_cast<uint8_t>(std::clamp(increment, 0, 31));

  int32_t target_priority = 0;
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
  if (!REXCVAR_GET(::rex::runtime::cvars::ignore_thread_priorities)) {
    thread_->set_priority(target_priority);
  }
}

void ThreadRecord::SetAffinity(uint32_t affinity) {
  const u8 mask = static_cast<u8>(affinity);
  if (mask) {
    SetActiveCPU(ProcessorFromMask(mask));
  }
}

uint8_t ThreadRecord::active_cpu() const {
  // Prefer reading from guest KTHREAD (always available for guest threads,
  // kept in sync by SetActiveCPU). Avoids dependency on pcr_address_ which
  // may not be set yet if the thread is mid-creation.
  if (is_guest_thread()) {
    auto* kthread = memory()->TranslateVirtual<const X_KTHREAD*>(guest_object());
    return kthread->current_cpu;
  }
  if (!pcr_address_) {
    return 0;
  }
  const X_KPCR& pcr = *memory()->TranslateVirtual<const X_KPCR*>(pcr_address_);
  return pcr.prcb_data.current_cpu;
}

void ThreadRecord::SetActiveCPU(uint8_t cpu_index) {
  // May be called during thread creation - don't skip if current == new.

  assert_true(cpu_index < 6);

  // Write to guest KTHREAD (always available for guest threads).
  if (is_guest_thread()) {
    X_KTHREAD& thread_object = *memory()->TranslateVirtual<X_KTHREAD*>(guest_object());
    thread_object.current_cpu = cpu_index;
  }

  // Write to PCR if allocated (may not be during early creation).
  if (pcr_address_) {
    X_KPCR& pcr = *memory()->TranslateVirtual<X_KPCR*>(pcr_address_);
    pcr.prcb_data.current_cpu = cpu_index;
  }

  if (rex::platform::logical_processor_count() >= 6) {
    if (!REXCVAR_GET(::rex::runtime::cvars::ignore_thread_affinities)) {
      thread_->set_affinity_mask(uint64_t(1) << cpu_index);
    }
  } else {
    REXLOG_WARN(system, "Too few processor cores - scheduling will be wonky");
  }
}

bool ThreadRecord::GetTLSValue(uint32_t slot, uint32_t* value_out) const {
  if (slot >= tls_slot_count_.load(std::memory_order_acquire)) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = memory::load_and_swap<uint32_t>(mem);
  return true;
}

// NOLINTNEXTLINE(readability-make-member-function-const): writes guest memory
bool ThreadRecord::SetTLSValue(uint32_t slot, uint32_t value) {
  if (slot >= tls_slot_count_.load(std::memory_order_acquire)) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  memory::store_and_swap<uint32_t>(mem, value);
  return true;
}

uint32_t ThreadRecord::suspend_count() {
  return guest_object<X_KTHREAD>()->suspend_count;
}

X_STATUS ThreadRecord::Resume(uint32_t* out_suspend_count) {
  auto guest_thread = guest_object<X_KTHREAD>();
  uint32_t unused_host_suspend_count = 0;

#if REX_PLATFORM_WIN32
  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)->fetch_sub(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  return thread_->Resume(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  std::lock_guard<std::mutex> lock(suspend_mutex_);
  const u8 previous = guest_thread->suspend_count;
  if (previous > 0) {
    guest_thread->suspend_count--;
  }
  if (out_suspend_count) {
    *out_suspend_count = previous;
  }
  suspend_cv_.notify_all();
  if (previous > 0) {
    thread_->Resume(&unused_host_suspend_count);
  }
  return X_STATUS_SUCCESS;
#else
  uint8_t previous_suspend_count = guest_thread->suspend_count;
  if (guest_thread->suspend_count > 0) {
    --guest_thread->suspend_count;
  }
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  return thread_->Resume(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
#endif
}

X_STATUS ThreadRecord::Suspend(uint32_t* out_suspend_count) {
  const bool self = IsInThread(this);
  auto guest_thread = guest_object<X_KTHREAD>();
  uint32_t unused_host_suspend_count = 0;
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  if (self) {
    const u32 previous = SelfSuspend();
    if (out_suspend_count) {
      *out_suspend_count = previous;
    }
    return X_STATUS_SUCCESS;
  }
  std::lock_guard<std::mutex> lock(suspend_mutex_);
  const u8 previous = guest_thread->suspend_count;
  guest_thread->suspend_count = static_cast<u8>(previous + 1);
  if (out_suspend_count) {
    *out_suspend_count = previous;
  }
  if (guest_thread->suspend_count == 0) {
    return X_STATUS_SUCCESS;
  }
  return thread_->Suspend(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
#else
  if (self) {
    CheckTitleTermination();
  }
  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)->fetch_add(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }

  // Wrapped to 0 - treat as not suspended.
  if (guest_thread->suspend_count == 0) {
    return X_STATUS_SUCCESS;
  }
  const bool suspended = thread_->Suspend(&unused_host_suspend_count);
  if (self) {
    CheckTitleTermination();
  }
  return suspended ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
#endif
}

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
uint32_t ThreadRecord::SelfSuspend() {
  CheckTitleTermination();
  auto guest_thread = guest_object<X_KTHREAD>();
  uint32_t previous = 0;
  {
    std::unique_lock<std::mutex> lock(suspend_mutex_);
    previous = guest_thread->suspend_count;
    guest_thread->suspend_count++;
    suspend_cv_.wait(lock, [guest_thread]() { return guest_thread->suspend_count == 0; });
  }
  CheckTitleTermination();
  return previous;
}
#endif

// NOLINTNEXTLINE(readability-make-member-function-const): waits on the host thread
X_STATUS ThreadRecord::Delay(uint32_t processor_mode, uint32_t alertable, uint64_t interval) {
  const u32 timeout_ms =
      Clock::ScaleGuestDurationMillis(TimeoutTicksToMs(static_cast<i64>(interval)));
  CheckTitleTermination();
  NoteKernelTime();
  if (alertable) {
    auto result = rex::platform::AlertableSleep(std::chrono::milliseconds(timeout_ms));
    CheckTitleTermination();
    switch (result) {
      default:
      case rex::platform::SleepResult::kSuccess:
        return X_STATUS_SUCCESS;
      case rex::platform::SleepResult::kAlerted:
        return X_STATUS_USER_APC;
    }
  } else {
    if (timeout_ms == 0) {
      if (priority_ <= rex::platform::ThreadPriority::kBelowNormal) {
        rex::platform::Sleep(std::chrono::microseconds(100));
      } else {
        rex::platform::MaybeYield();
      }
    } else {
      rex::platform::Sleep(std::chrono::milliseconds(timeout_ms));
    }
    CheckTitleTermination();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::runtime
