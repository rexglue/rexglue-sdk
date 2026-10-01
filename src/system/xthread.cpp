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
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/system/thread_registry.h>
#include <rex/system/xthread.h>
#include <rex/platform/thread.h>
#include <rex/vec128.h>
#include "system/cvar_table.h"

namespace rex::system {

const uint32_t XAPC::kSize;
const uint32_t XAPC::kDummyKernelRoutine;
const uint32_t XAPC::kDummyRundownRoutine;

ThreadRecord::ThreadRecord(KernelState* kernel_state, uint32_t stack_size,
                           uint32_t xapi_thread_startup, uint32_t start_address,
                           uint32_t start_context, uint32_t creation_flags, bool guest_thread,
                           bool main_thread, uint32_t guest_process)
    : thread_id_(kernel_state->threads()->NextID()),
      guest_thread_(guest_thread),
      main_thread_(main_thread),
      apc_lock_old_irql_(0),
      kernel_state_(kernel_state) {
  creation_params_.stack_size = stack_size;
  creation_params_.xapi_thread_startup = xapi_thread_startup;
  creation_params_.start_address = start_address;
  creation_params_.start_context = start_context;

  // top 8 bits = processor ID (or 0 for default)
  // bit 0 = 1 to create suspended
  creation_params_.creation_flags = creation_flags;
  creation_params_.guest_process = guest_process;

  // Adjust stack size - min of 16k.
  if (creation_params_.stack_size < 16 * 1024) {
    creation_params_.stack_size = 16 * 1024;
  }

  exit_gate_ = rex::platform::Event::CreateManualResetEvent(false);
}

ThreadRecord::ThreadRecord(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
                           std::function<int()> host_fn)
    : ThreadRecord(kernel_state, stack_size, 0, 0, 0, creation_flags, false) {
  host_fn_ = std::move(host_fn);
}

ThreadRecord::~ThreadRecord() {
  if (main_fiber_) {
    main_fiber_->Destroy();
    main_fiber_ = nullptr;
  }

  thread_.reset();

  kernel_state_->memory()->SystemHeapFree(scratch_address_);
  kernel_state_->memory()->SystemHeapFree(tls_static_address_);
  kernel_state_->memory()->SystemHeapFree(pcr_address_);
  FreeStack();
  FreeGuestBlock(memory(), guest_object_ptr_);
}

memory::Memory* ThreadRecord::memory() const {
  return kernel_state_->memory();
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
  if (self && self->is_guest_thread() && self->is_running() &&
      self->kernel_state()->is_terminating_title()) {
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

static uint8_t next_cpu = 0;
static uint8_t GetFakeCpuNumber(uint8_t proc_mask) {
  // NOTE: proc_mask is logical processors, not physical processors or cores.
  if (!proc_mask) {
    next_cpu = (next_cpu + 1) % 6;
    return next_cpu;  // is this reasonable?
    // TODO(Triang3l): Does the following apply here?
    // https://docs.microsoft.com/en-us/windows/win32/dxtecharts/coding-for-multiple-cores
    // "On Xbox 360, you must explicitly assign software threads to a particular
    //  hardware thread by using XSetThreadProcessor. Otherwise, all child
    //  threads will stay on the same hardware thread as the parent."
  }
  assert_false(proc_mask & 0xC0);

  uint8_t cpu_number = 7 - rex::lzcnt(proc_mask);
  assert_true(1 << cpu_number == proc_mask);
  assert_true(cpu_number < 6);
  return cpu_number;
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

  // Set process pointer - use guest_process if provided, else default to title process.
  uint32_t process_ptr = creation_params_.guest_process
                             ? creation_params_.guest_process
                             : kernel_state_->process_info_block_address();
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

    auto old_irql =
        kernel::xboxkrnl::KeKfAcquireSpinLock(ctx, &target_process->thread_list_spinlock);
    util::XeInsertTailList(&target_process->thread_list, &guest_thread->process_threads, memory());
    target_process->thread_count = target_process->thread_count + 1;
    kernel::xboxkrnl::KeKfReleaseSpinLock(ctx, &target_process->thread_list_spinlock, old_irql);
  } else {
    guest_thread->process_type = X_PROCTYPE_USER;
    guest_thread->process_type_dup = X_PROCTYPE_USER;
  }

  guest_thread->msr_mask = 0xFDFFD7FF;
  // current_cpu is expected to be initialized externally via SetActiveCpu.
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

bool ThreadRecord::AllocateStack(uint32_t size) {
  auto heap = memory()->LookupHeap(kStackAddressRangeBegin);

  auto alignment = heap->page_size();
  auto padding = heap->page_size() * 2;  // Guard page size * 2
  size = rex::round_up(size, alignment);
  auto actual_size = size + padding;

  uint32_t address = 0;
  if (!heap->AllocRange(kStackAddressRangeBegin, kStackAddressRangeEnd, actual_size, alignment,
                        memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                        memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false,
                        &address)) {
    return false;
  }

  stack_alloc_base_ = address;
  stack_alloc_size_ = actual_size;
  stack_limit_ = address + (padding / 2);
  stack_base_ = stack_limit_ + size;

  // Initialize the stack with junk
  memory()->Fill(stack_alloc_base_, actual_size, 0xBE);

  // Setup the guard pages
  heap->Protect(stack_alloc_base_, padding / 2, memory::kMemoryProtectNoAccess);
  heap->Protect(stack_base_, padding / 2, memory::kMemoryProtectNoAccess);

  return true;
}

void ThreadRecord::FreeStack() {
  if (stack_alloc_base_) {
    auto heap = memory()->LookupHeap(kStackAddressRangeBegin);
    heap->Release(stack_alloc_base_);

    stack_alloc_base_ = 0;
    stack_alloc_size_ = 0;
    stack_base_ = 0;
    stack_limit_ = 0;
  }
}

X_STATUS ThreadRecord::Create() {
  // Allocate a stack.
  if (!AllocateStack(creation_params_.stack_size)) {
    return X_STATUS_NO_MEMORY;
  }

  // Allocate thread scratch.
  // This is used by interrupts/APCs/etc so we can round-trip pointers through.
  scratch_size_ = 4 * 16;
  scratch_address_ = memory()->SystemHeapAlloc(scratch_size_);

  // Allocate TLS block.
  // Games will specify a certain number of 4b slots that each thread will get.
  xex2_opt_tls_info* tls_header = nullptr;
  auto module =
      kernel_state_->object_table()->Get<ModuleObject>(kernel_state_->GetExecutableModule());
  if (module) {
    UserModuleGetOptHeader(*module, XEX_HEADER_TLS_INFO, &tls_header);
  }

  const uint32_t kDefaultTlsSlotCount = 1024;
  uint32_t tls_slots = kDefaultTlsSlotCount;
  uint32_t tls_extended_size = 0;
  if (tls_header && tls_header->slot_count) {
    tls_slots = tls_header->slot_count;
    tls_extended_size = tls_header->data_size;
  }

  // Allocate both the slots and the extended data.
  // Some TLS is compiled with the binary (declspec(thread)) vars. The game
  // will directly access those through 0(r13).
  uint32_t tls_slot_size = tls_slots * 4;
  tls_total_size_ = tls_slot_size + tls_extended_size;
  tls_static_address_ = memory()->SystemHeapAlloc(tls_total_size_);
  tls_dynamic_address_ = tls_static_address_ + tls_extended_size;
  if (!tls_static_address_) {
    REXLOG_WARN(system, "Unable to allocate thread local storage block");
    return X_STATUS_NO_MEMORY;
  }

  // Zero all of TLS.
  memory()->Fill(tls_static_address_, tls_total_size_, 0);
  if (tls_extended_size) {
    // If game has extended data, copy in the default values.
    assert_not_zero(tls_header->raw_data_address);
    memory()->Copy(tls_static_address_, tls_header->raw_data_address, tls_header->raw_data_size);
  }

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
  thread_state_ =
      std::make_unique<runtime::ThreadState>(thread_id_, stack_base_, pcr_address_, memory());

  REXLOG_DEV(system, "XThread{:08X} ({:X}) Stack: {:08X}-{:08X}", handle(), thread_id_,
             stack_limit_, stack_base_);

  uint8_t cpu_index = GetFakeCpuNumber(static_cast<uint8_t>(creation_params_.creation_flags >> 24));

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
  kernel_state_->object_table()->Retain(handle_);

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
  SetActiveCpu(cpu_index);

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  if ((creation_params_.creation_flags & X_CREATE_SUSPENDED) == 0) {
    // Start the thread now that we're all setup.
    thread_->Resume();
  }

  return X_STATUS_SUCCESS;
}

X_STATUS ThreadRecord::Exit(int exit_code) {
  // This may only be called on the thread itself.
  assert_true(ThreadRecord::GetCurrentThread() == this);

  // Terminate got here first and is about to kill this thread.
  if (exit_claimed_.exchange(true)) {
    rex::platform::Wait(exit_gate_.get(), false, std::chrono::milliseconds::max());
  }

  // Mark as terminated before running down APCs.
  auto kthread = guest_object<X_KTHREAD>();
  kthread->terminated = 1;

  // TODO(benvanik): dispatch events? waiters? etc?
  RundownAPCs();

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // Unlink thread from process thread list.
  uint32_t process_guest = thread->process;
  if (process_guest) {
    auto* ctx = thread_state_->context();
    auto kprocess = memory()->TranslateVirtual<X_KPROCESS*>(process_guest);
    auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(ctx, &kprocess->thread_list_spinlock);
    util::XeRemoveEntryList(&thread->process_threads, memory());
    kprocess->thread_count = kprocess->thread_count - 1;
    kernel::xboxkrnl::KeKfReleaseSpinLock(ctx, &kprocess->thread_list_spinlock, old_irql);
  }

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  // The guest stack belongs to the thread's execution lifetime, not the handle
  // lifetime. Games can keep thread handles after exit; holding the stack until
  // object destruction leaks the reserved stack range and eventually makes
  // ExCreateThread fail even though the old threads are no longer running.
  FreeStack();

  // NOTE: unless PlatformExit fails, expect it to never return!
  ReleaseSelf();

  // NOTE: this does not return!
  rex::platform::Thread::Exit(exit_code);
  return X_STATUS_SUCCESS;
}

X_STATUS ThreadRecord::Terminate(int exit_code) {
  // TODO(benvanik): inform the profiler that this thread is exiting.

  // Exit already tears the thread down.
  if (exit_claimed_.exchange(true)) {
    return X_STATUS_SUCCESS;
  }

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

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
  KernelState* kernel_state = kernel_state_;
  const X_HANDLE handle = handle_;
  running_ = false;
  if (IsInThread(this)) {
    current_thread_tls_ = nullptr;
  }
  kernel_state->threads()->NoteExited(*this);
  kernel_state->object_table()->Close(handle);
}

void ThreadRecord::Execute() {
  REXLOG_DEV(system, "Execute thid {} (handle={:08X}, '{}', native={:08X})", thread_id_, handle(),
             thread_name_, thread_->system_id());

  if (host_fn_) {
    Exit(host_fn_());
    return;
  }

  // Dispatch any APCs that were queued before the thread was created first.
  DeliverAPCs();

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

  // NOTE(tomc): JIT execution replaced with direct function calls
  // In rexglue, guest code is compiled ahead of time and called directly.
  // The start_address points to a 32bit guest address, for which the function
  // dispatcher maintains a lookup table to retrieve the host function pointer.
  auto* runtime = Runtime::instance();
  if (!runtime || !runtime->function_dispatcher()) {
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
  if (args.size() > 0)
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

  ctx->fpscr.InitHost();

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
  apc_lock_old_irql_ =
      kernel::xboxkrnl::KeKfAcquireSpinLock(thread_state_->context(), &kthread->apc_lock);
}

void ThreadRecord::UnlockApc(bool queue_delivery) {
  auto kthread = guest_object<X_KTHREAD>();
  auto mem = memory();
  bool needs_apc = !kthread->apc_lists[0].empty(mem) || !kthread->apc_lists[1].empty(mem);
  kernel::xboxkrnl::KeKfReleaseSpinLock(thread_state_->context(), &kthread->apc_lock,
                                        apc_lock_old_irql_);
  if (needs_apc && queue_delivery) {
    // Match Edge/Canary behavior: callback is only a wakeup hint.
    // User APC execution happens on alertable wait return paths.
    thread_->QueueUserCallback([]() {});
  }
}

void ThreadRecord::EnqueueApc(uint32_t normal_routine, uint32_t normal_context, uint32_t arg1,
                              uint32_t arg2) {
  uint32_t apc_ptr = memory()->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    REXLOG_WARN(system, "EnqueueApc: allocation failed (thid={}, normal={:08X})", thread_id_,
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

  if (!kernel::xboxkrnl::KeInsertQueueApc(apc, arg1, arg2, 0, queue_ctx)) {
    memory()->SystemHeapFree(apc_ptr);
    REXLOG_ERROR(system,
                 "EnqueueApc: queue rejected (thid={}, normal={:08X}, ctx={:08X}, arg1={:08X}, "
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
  auto* dispatcher = kernel_state_->function_dispatcher();

  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(ctx, &kthread->apc_lock);
  auto& user_apc_queue = kthread->apc_lists[1];

  while (!user_apc_queue.empty(mem) && kthread->apc_disable_count == 0) {
    REX_PERF_INC(kAPCQueueDepth);
    XAPC* apc = user_apc_queue.HeadObject(mem);
    uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
    bool needs_freeing = apc->kernel_routine != XAPC::kDummyKernelRoutine;

    util::XeRemoveEntryList(&apc->list_entry, mem);
    apc->enqueued = 0;

    kernel::xboxkrnl::KeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);

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

    old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(ctx, &kthread->apc_lock);
  }

  kernel::xboxkrnl::KeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);
}

void ThreadRecord::RundownAPCs() {
  assert_true(ThreadRecord::GetCurrentThread() == this);
  auto mem = memory();
  auto kthread = guest_object<X_KTHREAD>();
  auto* ctx = thread_state_->context();

  // Rundown both user (1) and kernel (0) APC lists.
  for (int mode = 1; mode >= 0; --mode) {
    auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(ctx, &kthread->apc_lock);
    auto& apc_queue = kthread->apc_lists[mode];

    while (!apc_queue.empty(mem)) {
      XAPC* apc = apc_queue.HeadObject(mem);
      uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
      bool needs_freeing = apc->kernel_routine == XAPC::kDummyKernelRoutine;

      util::XeRemoveEntryList(&apc->list_entry, mem);
      apc->enqueued = 0;

      kernel::xboxkrnl::KeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);

      if (apc->rundown_routine == XAPC::kDummyRundownRoutine) {
        // No-op.
      } else if (apc->rundown_routine) {
        auto* dispatcher = kernel_state_->function_dispatcher();
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

      old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(ctx, &kthread->apc_lock);
    }
    kernel::xboxkrnl::KeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);
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
  SetActiveCpu(GetFakeCpuNumber(affinity));
}

uint8_t ThreadRecord::active_cpu() const {
  // Prefer reading from guest KTHREAD (always available for guest threads,
  // kept in sync by SetActiveCpu). Avoids dependency on pcr_address_ which
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

void ThreadRecord::SetActiveCpu(uint8_t cpu_index) {
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

bool ThreadRecord::GetTLSValue(uint32_t slot, uint32_t* value_out) {
  if (slot * 4 > tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = memory::load_and_swap<uint32_t>(mem);
  return true;
}

bool ThreadRecord::SetTLSValue(uint32_t slot, uint32_t value) {
  if (slot * 4 >= tls_total_size_) {
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
  bool should_resume_host = false;
  {
    std::lock_guard<std::mutex> lock(suspend_mutex_);
    uint8_t previous = guest_thread->suspend_count;
    if (previous > 0) {
      guest_thread->suspend_count--;
    }
    if (out_suspend_count) {
      *out_suspend_count = previous;
    }
    should_resume_host = (guest_thread->suspend_count == 0);
    suspend_cv_.notify_all();
  }

  // Self-suspended threads are resumed via guest suspend count transitions.
  if (should_resume_host) {
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
  auto guest_thread = guest_object<X_KTHREAD>();
  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)->fetch_add(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }

  uint32_t unused_host_suspend_count = 0;
  // Wrapped to 0 - treat as not suspended.
  if (guest_thread->suspend_count == 0) {
    return X_STATUS_SUCCESS;
  }
  return thread_->Suspend(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
uint32_t ThreadRecord::SelfSuspend() {
  auto guest_thread = guest_object<X_KTHREAD>();
  std::unique_lock<std::mutex> lock(suspend_mutex_);
  uint32_t previous = guest_thread->suspend_count;
  guest_thread->suspend_count++;
  suspend_cv_.wait(lock, [guest_thread]() { return guest_thread->suspend_count == 0; });
  return previous;
}
#endif

X_STATUS ThreadRecord::Delay(uint32_t processor_mode, uint32_t alertable, uint64_t interval) {
  int64_t timeout_ticks = interval;
  uint32_t timeout_ms;
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    timeout_ms = 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    timeout_ms = uint32_t(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    timeout_ms = 0;
  }
  timeout_ms = Clock::ScaleGuestDurationMillis(timeout_ms);
  CheckTitleTermination();
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

struct ThreadSavedState {
  uint32_t thread_id;
  bool is_main_thread;  // Is this the main thread?
  bool is_running;

  uint32_t apc_head;
  uint32_t tls_static_address;
  uint32_t tls_dynamic_address;
  uint32_t tls_total_size;
  uint32_t pcr_address;
  uint32_t stack_base;        // High address
  uint32_t stack_limit;       // Low address
  uint32_t stack_alloc_base;  // Allocation address
  uint32_t stack_alloc_size;  // Allocation size

  // Context (invalid if not running)
  struct {
    uint64_t lr;
    uint64_t ctr;
    uint64_t r[32];
    double f[32];
    vec128_t v[128];
    uint32_t cr[8];
    uint32_t fpscr;
    uint8_t xer_ca;
    uint8_t xer_ov;
    uint8_t xer_so;
    uint8_t vscr_sat;
    uint32_t pc;
  } context;
};

// Save PPCContext to ThreadSavedState
static void SaveContext(const PPCContext* ctx, ThreadSavedState& state) {
  state.context.lr = ctx->lr;
  state.context.ctr = ctx->ctr.u64;

  // GPRs - copy individually since PPCContext uses named registers
  state.context.r[0] = ctx->r0.u64;
  state.context.r[1] = ctx->r1.u64;
  state.context.r[2] = ctx->r2.u64;
  state.context.r[3] = ctx->r3.u64;
  state.context.r[4] = ctx->r4.u64;
  state.context.r[5] = ctx->r5.u64;
  state.context.r[6] = ctx->r6.u64;
  state.context.r[7] = ctx->r7.u64;
  state.context.r[8] = ctx->r8.u64;
  state.context.r[9] = ctx->r9.u64;
  state.context.r[10] = ctx->r10.u64;
  state.context.r[11] = ctx->r11.u64;
  state.context.r[12] = ctx->r12.u64;
  state.context.r[13] = ctx->r13.u64;
  // r14-r31: contiguous in PPCContext and state array
  std::memcpy(&state.context.r[14], &ctx->r14, 18 * sizeof(uint64_t));

  // FPRs
  state.context.f[0] = ctx->f0.f64;
  state.context.f[1] = ctx->f1.f64;
  state.context.f[2] = ctx->f2.f64;
  state.context.f[3] = ctx->f3.f64;
  state.context.f[4] = ctx->f4.f64;
  state.context.f[5] = ctx->f5.f64;
  state.context.f[6] = ctx->f6.f64;
  state.context.f[7] = ctx->f7.f64;
  state.context.f[8] = ctx->f8.f64;
  state.context.f[9] = ctx->f9.f64;
  state.context.f[10] = ctx->f10.f64;
  state.context.f[11] = ctx->f11.f64;
  state.context.f[12] = ctx->f12.f64;
  state.context.f[13] = ctx->f13.f64;
  // f14-f31: contiguous in PPCContext and state array
  std::memcpy(&state.context.f[14], &ctx->f14, 18 * sizeof(double));

  // VRs (v0-v127)
  std::memcpy(&state.context.v[0], &ctx->v0, sizeof(vec128_t));
  std::memcpy(&state.context.v[1], &ctx->v1, sizeof(vec128_t));
  std::memcpy(&state.context.v[2], &ctx->v2, sizeof(vec128_t));
  std::memcpy(&state.context.v[3], &ctx->v3, sizeof(vec128_t));
  std::memcpy(&state.context.v[4], &ctx->v4, sizeof(vec128_t));
  std::memcpy(&state.context.v[5], &ctx->v5, sizeof(vec128_t));
  std::memcpy(&state.context.v[6], &ctx->v6, sizeof(vec128_t));
  std::memcpy(&state.context.v[7], &ctx->v7, sizeof(vec128_t));
  std::memcpy(&state.context.v[8], &ctx->v8, sizeof(vec128_t));
  std::memcpy(&state.context.v[9], &ctx->v9, sizeof(vec128_t));
  std::memcpy(&state.context.v[10], &ctx->v10, sizeof(vec128_t));
  std::memcpy(&state.context.v[11], &ctx->v11, sizeof(vec128_t));
  std::memcpy(&state.context.v[12], &ctx->v12, sizeof(vec128_t));
  std::memcpy(&state.context.v[13], &ctx->v13, sizeof(vec128_t));
  // v14-v31: contiguous in PPCContext and state array
  std::memcpy(&state.context.v[14], &ctx->v14, 18 * sizeof(vec128_t));
  std::memcpy(&state.context.v[32], &ctx->v32, sizeof(vec128_t));
  std::memcpy(&state.context.v[33], &ctx->v33, sizeof(vec128_t));
  std::memcpy(&state.context.v[34], &ctx->v34, sizeof(vec128_t));
  std::memcpy(&state.context.v[35], &ctx->v35, sizeof(vec128_t));
  std::memcpy(&state.context.v[36], &ctx->v36, sizeof(vec128_t));
  std::memcpy(&state.context.v[37], &ctx->v37, sizeof(vec128_t));
  std::memcpy(&state.context.v[38], &ctx->v38, sizeof(vec128_t));
  std::memcpy(&state.context.v[39], &ctx->v39, sizeof(vec128_t));
  std::memcpy(&state.context.v[40], &ctx->v40, sizeof(vec128_t));
  std::memcpy(&state.context.v[41], &ctx->v41, sizeof(vec128_t));
  std::memcpy(&state.context.v[42], &ctx->v42, sizeof(vec128_t));
  std::memcpy(&state.context.v[43], &ctx->v43, sizeof(vec128_t));
  std::memcpy(&state.context.v[44], &ctx->v44, sizeof(vec128_t));
  std::memcpy(&state.context.v[45], &ctx->v45, sizeof(vec128_t));
  std::memcpy(&state.context.v[46], &ctx->v46, sizeof(vec128_t));
  std::memcpy(&state.context.v[47], &ctx->v47, sizeof(vec128_t));
  std::memcpy(&state.context.v[48], &ctx->v48, sizeof(vec128_t));
  std::memcpy(&state.context.v[49], &ctx->v49, sizeof(vec128_t));
  std::memcpy(&state.context.v[50], &ctx->v50, sizeof(vec128_t));
  std::memcpy(&state.context.v[51], &ctx->v51, sizeof(vec128_t));
  std::memcpy(&state.context.v[52], &ctx->v52, sizeof(vec128_t));
  std::memcpy(&state.context.v[53], &ctx->v53, sizeof(vec128_t));
  std::memcpy(&state.context.v[54], &ctx->v54, sizeof(vec128_t));
  std::memcpy(&state.context.v[55], &ctx->v55, sizeof(vec128_t));
  std::memcpy(&state.context.v[56], &ctx->v56, sizeof(vec128_t));
  std::memcpy(&state.context.v[57], &ctx->v57, sizeof(vec128_t));
  std::memcpy(&state.context.v[58], &ctx->v58, sizeof(vec128_t));
  std::memcpy(&state.context.v[59], &ctx->v59, sizeof(vec128_t));
  std::memcpy(&state.context.v[60], &ctx->v60, sizeof(vec128_t));
  std::memcpy(&state.context.v[61], &ctx->v61, sizeof(vec128_t));
  std::memcpy(&state.context.v[62], &ctx->v62, sizeof(vec128_t));
  std::memcpy(&state.context.v[63], &ctx->v63, sizeof(vec128_t));
  // v64-v127: contiguous in PPCContext and state array
  std::memcpy(&state.context.v[64], &ctx->v64, 64 * sizeof(vec128_t));

  // CR fields
  state.context.cr[0] = ctx->cr0.raw();
  state.context.cr[1] = ctx->cr1.raw();
  state.context.cr[2] = ctx->cr2.raw();
  state.context.cr[3] = ctx->cr3.raw();
  state.context.cr[4] = ctx->cr4.raw();
  state.context.cr[5] = ctx->cr5.raw();
  state.context.cr[6] = ctx->cr6.raw();
  state.context.cr[7] = ctx->cr7.raw();

  // Other state
  state.context.fpscr = ctx->fpscr.csr;
  state.context.xer_ca = ctx->xer.ca;
  state.context.xer_ov = ctx->xer.ov;
  state.context.xer_so = ctx->xer.so;
  state.context.vscr_sat = ctx->vscr_sat;
}

// Load ThreadSavedState into PPCContext
static void LoadContext(PPCContext* ctx, const ThreadSavedState& state) {
  ctx->lr = state.context.lr;
  ctx->ctr.u64 = state.context.ctr;

  // GPRs
  ctx->r0.u64 = state.context.r[0];
  ctx->r1.u64 = state.context.r[1];
  ctx->r2.u64 = state.context.r[2];
  ctx->r3.u64 = state.context.r[3];
  ctx->r4.u64 = state.context.r[4];
  ctx->r5.u64 = state.context.r[5];
  ctx->r6.u64 = state.context.r[6];
  ctx->r7.u64 = state.context.r[7];
  ctx->r8.u64 = state.context.r[8];
  ctx->r9.u64 = state.context.r[9];
  ctx->r10.u64 = state.context.r[10];
  ctx->r11.u64 = state.context.r[11];
  ctx->r12.u64 = state.context.r[12];
  ctx->r13.u64 = state.context.r[13];
  // r14-r31: contiguous in PPCContext and state array
  std::memcpy(&ctx->r14, &state.context.r[14], 18 * sizeof(uint64_t));

  // FPRs
  ctx->f0.f64 = state.context.f[0];
  ctx->f1.f64 = state.context.f[1];
  ctx->f2.f64 = state.context.f[2];
  ctx->f3.f64 = state.context.f[3];
  ctx->f4.f64 = state.context.f[4];
  ctx->f5.f64 = state.context.f[5];
  ctx->f6.f64 = state.context.f[6];
  ctx->f7.f64 = state.context.f[7];
  ctx->f8.f64 = state.context.f[8];
  ctx->f9.f64 = state.context.f[9];
  ctx->f10.f64 = state.context.f[10];
  ctx->f11.f64 = state.context.f[11];
  ctx->f12.f64 = state.context.f[12];
  ctx->f13.f64 = state.context.f[13];
  // f14-f31: contiguous in PPCContext and state array
  std::memcpy(&ctx->f14, &state.context.f[14], 18 * sizeof(double));

  // VRs (v0-v127)
  std::memcpy(&ctx->v0, &state.context.v[0], sizeof(vec128_t));
  std::memcpy(&ctx->v1, &state.context.v[1], sizeof(vec128_t));
  std::memcpy(&ctx->v2, &state.context.v[2], sizeof(vec128_t));
  std::memcpy(&ctx->v3, &state.context.v[3], sizeof(vec128_t));
  std::memcpy(&ctx->v4, &state.context.v[4], sizeof(vec128_t));
  std::memcpy(&ctx->v5, &state.context.v[5], sizeof(vec128_t));
  std::memcpy(&ctx->v6, &state.context.v[6], sizeof(vec128_t));
  std::memcpy(&ctx->v7, &state.context.v[7], sizeof(vec128_t));
  std::memcpy(&ctx->v8, &state.context.v[8], sizeof(vec128_t));
  std::memcpy(&ctx->v9, &state.context.v[9], sizeof(vec128_t));
  std::memcpy(&ctx->v10, &state.context.v[10], sizeof(vec128_t));
  std::memcpy(&ctx->v11, &state.context.v[11], sizeof(vec128_t));
  std::memcpy(&ctx->v12, &state.context.v[12], sizeof(vec128_t));
  std::memcpy(&ctx->v13, &state.context.v[13], sizeof(vec128_t));
  // v14-v31: contiguous in PPCContext and state array
  std::memcpy(&ctx->v14, &state.context.v[14], 18 * sizeof(vec128_t));
  std::memcpy(&ctx->v32, &state.context.v[32], sizeof(vec128_t));
  std::memcpy(&ctx->v33, &state.context.v[33], sizeof(vec128_t));
  std::memcpy(&ctx->v34, &state.context.v[34], sizeof(vec128_t));
  std::memcpy(&ctx->v35, &state.context.v[35], sizeof(vec128_t));
  std::memcpy(&ctx->v36, &state.context.v[36], sizeof(vec128_t));
  std::memcpy(&ctx->v37, &state.context.v[37], sizeof(vec128_t));
  std::memcpy(&ctx->v38, &state.context.v[38], sizeof(vec128_t));
  std::memcpy(&ctx->v39, &state.context.v[39], sizeof(vec128_t));
  std::memcpy(&ctx->v40, &state.context.v[40], sizeof(vec128_t));
  std::memcpy(&ctx->v41, &state.context.v[41], sizeof(vec128_t));
  std::memcpy(&ctx->v42, &state.context.v[42], sizeof(vec128_t));
  std::memcpy(&ctx->v43, &state.context.v[43], sizeof(vec128_t));
  std::memcpy(&ctx->v44, &state.context.v[44], sizeof(vec128_t));
  std::memcpy(&ctx->v45, &state.context.v[45], sizeof(vec128_t));
  std::memcpy(&ctx->v46, &state.context.v[46], sizeof(vec128_t));
  std::memcpy(&ctx->v47, &state.context.v[47], sizeof(vec128_t));
  std::memcpy(&ctx->v48, &state.context.v[48], sizeof(vec128_t));
  std::memcpy(&ctx->v49, &state.context.v[49], sizeof(vec128_t));
  std::memcpy(&ctx->v50, &state.context.v[50], sizeof(vec128_t));
  std::memcpy(&ctx->v51, &state.context.v[51], sizeof(vec128_t));
  std::memcpy(&ctx->v52, &state.context.v[52], sizeof(vec128_t));
  std::memcpy(&ctx->v53, &state.context.v[53], sizeof(vec128_t));
  std::memcpy(&ctx->v54, &state.context.v[54], sizeof(vec128_t));
  std::memcpy(&ctx->v55, &state.context.v[55], sizeof(vec128_t));
  std::memcpy(&ctx->v56, &state.context.v[56], sizeof(vec128_t));
  std::memcpy(&ctx->v57, &state.context.v[57], sizeof(vec128_t));
  std::memcpy(&ctx->v58, &state.context.v[58], sizeof(vec128_t));
  std::memcpy(&ctx->v59, &state.context.v[59], sizeof(vec128_t));
  std::memcpy(&ctx->v60, &state.context.v[60], sizeof(vec128_t));
  std::memcpy(&ctx->v61, &state.context.v[61], sizeof(vec128_t));
  std::memcpy(&ctx->v62, &state.context.v[62], sizeof(vec128_t));
  std::memcpy(&ctx->v63, &state.context.v[63], sizeof(vec128_t));
  // v64-v127: contiguous in PPCContext and state array
  std::memcpy(&ctx->v64, &state.context.v[64], 64 * sizeof(vec128_t));

  // CR fields
  ctx->cr0.set_raw(state.context.cr[0]);
  ctx->cr1.set_raw(state.context.cr[1]);
  ctx->cr2.set_raw(state.context.cr[2]);
  ctx->cr3.set_raw(state.context.cr[3]);
  ctx->cr4.set_raw(state.context.cr[4]);
  ctx->cr5.set_raw(state.context.cr[5]);
  ctx->cr6.set_raw(state.context.cr[6]);
  ctx->cr7.set_raw(state.context.cr[7]);

  // Other state
  ctx->fpscr.csr = state.context.fpscr;
  ctx->xer.ca = state.context.xer_ca;
  ctx->xer.ov = state.context.xer_ov;
  ctx->xer.so = state.context.xer_so;
  ctx->vscr_sat = state.context.vscr_sat;
}

}  // namespace rex::system
