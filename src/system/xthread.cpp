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
#include <cstring>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/literals.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/ppc/exceptions.h>
#include <rex/runtime.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/processor.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/system/xevent.h>
#include <rex/system/xmutant.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(ignore_thread_priorities, true, "Kernel",
                    "Ignores game-specified thread priorities");

REXCVAR_DEFINE_BOOL(ignore_thread_affinities, true, "Kernel",
                    "Ignores game-specified thread affinities");

namespace rex::system {

const uint32_t XAPC::kSize;
const uint32_t XAPC::kDummyKernelRoutine;
const uint32_t XAPC::kDummyRundownRoutine;

using namespace rex::literals;

uint32_t next_xthread_id_ = 0;

XThread::XThread(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType), guest_thread_(true) {}

XThread::XThread(KernelState* kernel_state, uint32_t stack_size, uint32_t xapi_thread_startup,
                 uint32_t start_address, uint32_t start_context, uint32_t creation_flags,
                 bool guest_thread, bool main_thread, uint32_t guest_process)
    : XObject(kernel_state, kObjectType),
      thread_id_(++next_xthread_id_),
      guest_thread_(guest_thread),
      main_thread_(main_thread),
      apc_lock_old_irql_(0) {
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

  if (!guest_thread_) {
    host_object_ = true;
  }

  // The kernel does not take a reference. We must unregister in the dtor.
  kernel_state_->RegisterThread(this);
}

XThread::~XThread() {
  // Unregister first to prevent lookups while deleting.
  kernel_state_->UnregisterThread(this);

  thread_.reset();

  kernel_state()->memory()->SystemHeapFree(scratch_address_);
  kernel_state()->memory()->SystemHeapFree(tls_static_address_);
  kernel_state()->memory()->SystemHeapFree(pcr_address_);
  FreeStack();

  if (thread_) {
    // TODO(benvanik): platform kill
    REXSYS_ERROR("Thread disposed without exiting");
  }
}

thread_local XThread* current_xthread_tls_ = nullptr;

bool XThread::IsInThread() {
  return current_xthread_tls_ != nullptr;
}

bool XThread::IsInThread(XThread* other) {
  return current_xthread_tls_ == other;
}

XThread* XThread::GetCurrentThread() {
  XThread* thread = reinterpret_cast<XThread*>(current_xthread_tls_);
  if (!thread) {
    assert_always("Attempting to use kernel stuff from a non-kernel thread");
  }
  return thread;
}

uint32_t XThread::GetCurrentThreadHandle() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->handle();
}

uint32_t XThread::GetCurrentThreadId() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->guest_object<X_KTHREAD>()->thread_id;
}

uint32_t XThread::GetLastError() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->last_error();
}

void XThread::SetLastError(uint32_t error_code) {
  XThread* thread = XThread::GetCurrentThread();
  thread->set_last_error(error_code);
}

uint32_t XThread::last_error() {
  return guest_object<X_KTHREAD>()->last_error;
}

void XThread::set_last_error(uint32_t error_code) {
  guest_object<X_KTHREAD>()->last_error = error_code;
}

void XThread::set_name(const std::string_view name) {
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

void XThread::InitializeGuestObject() {
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
  guest_thread->tls_address = tls_static_address_;
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

    auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(
        ctx, &target_process->thread_list_spinlock);
    util::XeInsertTailList(&target_process->thread_list,
                           &guest_thread->process_threads, memory());
    target_process->thread_count = target_process->thread_count + 1;
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(
        ctx, &target_process->thread_list_spinlock, old_irql);
  } else {
    guest_thread->process_type = X_PROCTYPE_USER;
    guest_thread->process_type_dup = X_PROCTYPE_USER;
  }

  guest_thread->msr_mask = 0xFDFFD7FF;
  // current_cpu is expected to be initialized externally via SetActiveCpu.
  guest_thread->stack_alloc_base = stack_base_;
  guest_thread->create_time = chrono::Clock::QueryGuestSystemTime();

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

bool XThread::AllocateStack(uint32_t size) {
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

void XThread::FreeStack() {
  if (stack_alloc_base_) {
    auto heap = memory()->LookupHeap(kStackAddressRangeBegin);
    heap->Release(stack_alloc_base_);

    stack_alloc_base_ = 0;
    stack_alloc_size_ = 0;
    stack_base_ = 0;
    stack_limit_ = 0;
  }
}

X_STATUS XThread::Create() {
  // Thread kernel object.
  if (!CreateNative<X_KTHREAD>()) {
    REXSYS_WARN("Unable to allocate thread object");
    return X_STATUS_NO_MEMORY;
  }

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
  auto module = kernel_state()->GetExecutableModule();
  if (module) {
    module->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
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
    REXSYS_WARN("Unable to allocate thread local storage block");
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
    REXSYS_WARN("Unable to allocate thread state block");
    return X_STATUS_NO_MEMORY;
  }

  // Create thread state - needed for interrupt callbacks and kernel exports
  thread_state_ =
      std::make_unique<runtime::ThreadState>(thread_id_, stack_base_, pcr_address_, memory());

  // Set kernel state in context for kernel callbacks
  thread_state_->context()->kernel_state = kernel_state_;

  REXSYS_DEBUG("XThread{:08X} ({:X}) Stack: {:08X}-{:08X}", handle(), thread_id_, stack_limit_,
               stack_base_);

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
  RetainHandle();

  rex::thread::Thread::CreationParameters params;
  params.stack_size = 16_MiB;  // Allocate a big host stack.
  params.create_suspended = true;
  thread_ = rex::thread::Thread::Create(params, [this]() {
    rex::initialize_seh_thread();

    // Set thread ID override. This is used by logging.
    rex::thread::set_current_thread_id(handle());

    // Set name immediately, if we have one.
    thread_->set_name(thread_name_);

    PROFILE_THREAD_ENTER(thread_name_.c_str());

    // Execute user code.
    current_xthread_tls_ = this;
    running_ = true;
    Execute();
    running_ = false;
    current_xthread_tls_ = nullptr;

    PROFILE_THREAD_EXIT();

    // Release the self-reference to the thread.
    ReleaseHandle();
  });

  if (!thread_) {
    // TODO(benvanik): translate error?
    REXSYS_ERROR("CreateThread failed");
    return X_STATUS_NO_MEMORY;
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

X_STATUS XThread::Exit(int exit_code) {
  // This may only be called on the thread itself.
  assert_true(XThread::GetCurrentThread() == this);

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
    auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(
        ctx, &kprocess->thread_list_spinlock);
    util::XeRemoveEntryList(&thread->process_threads, memory());
    kprocess->thread_count = kprocess->thread_count - 1;
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(
        ctx, &kprocess->thread_list_spinlock, old_irql);
  }

  kernel_state()->OnThreadExit(this);

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  // NOTE: unless PlatformExit fails, expect it to never return!
  current_xthread_tls_ = nullptr;
  PROFILE_THREAD_EXIT();

  running_ = false;
  ReleaseHandle();

  // NOTE: this does not return!
  rex::thread::Thread::Exit(exit_code);
  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Terminate(int exit_code) {
  // TODO(benvanik): inform the profiler that this thread is exiting.

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  running_ = false;
  if (XThread::IsInThread(this)) {
    ReleaseHandle();
    rex::thread::Thread::Exit(exit_code);
  } else {
    thread_->Terminate(exit_code);
    ReleaseHandle();
  }

  return X_STATUS_SUCCESS;
}

class reenter_exception {
 public:
  reenter_exception(uint32_t address) : address_(address) {};
  virtual ~reenter_exception() {};
  uint32_t address() const { return address_; }

 private:
  uint32_t address_;
};

void XThread::Execute() {
  REXSYS_DEBUG("Execute thid {} (handle={:08X}, '{}', native={:08X})", thread_id_, handle(),
               thread_name_, thread_->system_id());

  // Let the kernel know we are starting.
  kernel_state()->OnThreadExecute(this);

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
  // The start_address points to a 32bit guest address, for which the processor
  // maintains a lookup table for to retrieve the host function pointer.
  auto* runtime = Runtime::instance();
  if (!runtime || !runtime->processor()) {
    REXSYS_ERROR("XThread::Execute - Runtime not initialized");
    return;
  }

  auto* processor = runtime->processor();
  auto* memory = runtime->memory();
  PPCFunc* func = processor->GetFunction(address);
  if (!func) {
    REXSYS_ERROR("XThread::Execute - No function registered at {:08X}", address);
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

  REXSYS_DEBUG("XThread::Execute - Calling function at {:08X}", address);
  func(*ctx, base);

  exit_code = static_cast<int>(ctx->r3.u32);

  // If we got here it means the execute completed without an exit being called.
  // Treat the return code as an implicit exit code (if desired).
  Exit(!want_exit_code ? 0 : exit_code);
}

void XThread::Reenter(uint32_t address) {
  // TODO(gibbed): Maybe use setjmp/longjmp on Windows?
  // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/longjmp#remarks
  // On Windows with /EH, setjmp/longjmp do stack unwinding.
  // Is there a better solution than exceptions for stack unwinding?
  throw reenter_exception(address);
}

void XThread::EnterCriticalRegion() {
  guest_object<X_KTHREAD>()->apc_disable_count--;
}

void XThread::LeaveCriticalRegion() {
  auto kthread = guest_object<X_KTHREAD>();
  auto apc_disable_count = ++kthread->apc_disable_count;
  if (apc_disable_count == 0) {
    // NOTE: intentionally not calling CheckApcs() here.
    // Delivering APCs here can cause them to fire in wrong contexts.
  }
}

void XThread::LockApc() {
  auto kthread = guest_object<X_KTHREAD>();
  apc_lock_old_irql_ = kernel::xboxkrnl::xeKeKfAcquireSpinLock(
      thread_state_->context(), &kthread->apc_lock);
}

void XThread::UnlockApc(bool queue_delivery) {
  auto kthread = guest_object<X_KTHREAD>();
  auto mem = memory();
  bool needs_apc =
      !kthread->apc_lists[0].empty(mem) || !kthread->apc_lists[1].empty(mem);
  kernel::xboxkrnl::xeKeKfReleaseSpinLock(
      thread_state_->context(), &kthread->apc_lock, apc_lock_old_irql_);
  if (needs_apc && queue_delivery) {
    thread_->QueueUserCallback([this]() { DeliverAPCs(); });
  }
}

void XThread::EnqueueApc(uint32_t normal_routine, uint32_t normal_context, uint32_t arg1,
                         uint32_t arg2) {
  uint32_t apc_ptr = memory()->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    return;
  }
  auto apc = memory()->TranslateVirtual<XAPC*>(apc_ptr);
  kernel::xboxkrnl::xeKeInitializeApc(apc, guest_object(), XAPC::kDummyKernelRoutine,
                                       XAPC::kDummyRundownRoutine, normal_routine,
                                       1 /* user apc mode */, normal_context);

  if (!kernel::xboxkrnl::xeKeInsertQueueApc(apc, arg1, arg2, 0,
                                             thread_state_->context())) {
    memory()->SystemHeapFree(apc_ptr);
    return;
  }
  thread_->QueueUserCallback([this]() { DeliverAPCs(); });
}

void XThread::DeliverAPCs() {
  auto mem = memory();
  auto kthread = guest_object<X_KTHREAD>();
  auto* ctx = thread_state_->context();

  // Process user APCs (list[1]).
  auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
  auto& user_apc_queue = kthread->apc_lists[1];

  while (!user_apc_queue.empty(mem) && kthread->apc_disable_count == 0) {
    XAPC* apc = user_apc_queue.HeadObject(mem);
    uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
    bool needs_freeing = apc->kernel_routine == XAPC::kDummyKernelRoutine;

    REXSYS_DEBUG("Delivering APC to {:08X}", uint32_t(apc->normal_routine));

    // Dequeue and mark as uninserted.
    util::XeRemoveEntryList(&apc->list_entry, mem);
    apc->enqueued = 0;

    kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);

    // Call kernel routine.
    // The routine can modify all of its arguments before passing it on.
    // Since we need to give guest accessible pointers over, we copy things
    // into and out of scratch.
    uint8_t* scratch_ptr = mem->TranslateVirtual(scratch_address_);
    memory::store_and_swap<uint32_t>(scratch_ptr + 0, apc->normal_routine);
    memory::store_and_swap<uint32_t>(scratch_ptr + 4, apc->normal_context);
    memory::store_and_swap<uint32_t>(scratch_ptr + 8, apc->arg1);
    memory::store_and_swap<uint32_t>(scratch_ptr + 12, apc->arg2);
    if (apc->kernel_routine != XAPC::kDummyKernelRoutine) {
      auto fn = kernel_state()->processor()->GetFunction(apc->kernel_routine);
      if (fn) {
        auto* ctx = thread_state_->context();
        ctx->r3.u64 = apc_ptr;
        ctx->r4.u64 = scratch_address_ + 0;
        ctx->r5.u64 = scratch_address_ + 4;
        ctx->r6.u64 = scratch_address_ + 8;
        ctx->r7.u64 = scratch_address_ + 12;
        fn(*ctx, mem->virtual_membase());
      } else {
        REXSYS_WARN("DeliverAPCs: kernel_routine {:08X} not found", uint32_t(apc->kernel_routine));
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
      auto fn = kernel_state()->processor()->GetFunction(normal_routine);
      if (fn) {
        auto* ctx = thread_state_->context();
        ctx->r3.u64 = normal_context;
        ctx->r4.u64 = arg1;
        ctx->r5.u64 = arg2;
        fn(*ctx, mem->virtual_membase());
      } else {
        REXSYS_WARN("DeliverAPCs: normal_routine {:08X} not found", normal_routine);
      }
    }

    REXSYS_DEBUG("Completed delivery of APC to {:08X} ({:08X}, {:08X}, {:08X})", normal_routine,
                 normal_context, arg1, arg2);

    if (needs_freeing) {
      mem->SystemHeapFree(apc_ptr);
    }

    old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
  }
  kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);
}

void XThread::RundownAPCs() {
  assert_true(XThread::GetCurrentThread() == this);
  auto mem = memory();
  auto kthread = guest_object<X_KTHREAD>();
  auto* ctx = thread_state_->context();

  // Rundown both user (1) and kernel (0) APC lists.
  for (int mode = 1; mode >= 0; --mode) {
    auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
    auto& apc_queue = kthread->apc_lists[mode];

    while (!apc_queue.empty(mem)) {
      XAPC* apc = apc_queue.HeadObject(mem);
      uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
      bool needs_freeing = apc->kernel_routine == XAPC::kDummyKernelRoutine;

      util::XeRemoveEntryList(&apc->list_entry, mem);
      apc->enqueued = 0;

      kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);

      if (apc->rundown_routine == XAPC::kDummyRundownRoutine) {
        // No-op.
      } else if (apc->rundown_routine) {
        auto fn = kernel_state()->processor()->GetFunction(apc->rundown_routine);
        if (fn) {
          auto* ctx = thread_state_->context();
          ctx->r3.u64 = apc_ptr;
          fn(*ctx, mem->virtual_membase());
        } else {
          REXSYS_WARN("RundownAPCs: rundown_routine {:08X} not found",
                      uint32_t(apc->rundown_routine));
        }
      }

      if (needs_freeing) {
        mem->SystemHeapFree(apc_ptr);
      }

      old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
    }
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);
  }
}

int32_t XThread::QueryPriority() {
  return thread_->priority();
}

void XThread::SetPriority(int32_t increment) {
  priority_ = increment;

  // Write priority to guest X_KTHREAD struct.
  auto kthread = guest_object<X_KTHREAD>();
  kthread->priority = static_cast<uint8_t>(std::clamp(increment, 0, 31));

  int32_t target_priority = 0;
  if (increment > 0x22) {
    target_priority = rex::thread::ThreadPriority::kHighest;
  } else if (increment > 0x11) {
    target_priority = rex::thread::ThreadPriority::kAboveNormal;
  } else if (increment < -0x22) {
    target_priority = rex::thread::ThreadPriority::kLowest;
  } else if (increment < -0x11) {
    target_priority = rex::thread::ThreadPriority::kBelowNormal;
  } else {
    target_priority = rex::thread::ThreadPriority::kNormal;
  }
  if (!REXCVAR_GET(ignore_thread_priorities)) {
    thread_->set_priority(target_priority);
  }
}

void XThread::SetAffinity(uint32_t affinity) {
  SetActiveCpu(GetFakeCpuNumber(affinity));
}

uint8_t XThread::active_cpu() const {
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

void XThread::SetActiveCpu(uint8_t cpu_index) {
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

  if (rex::thread::logical_processor_count() >= 6) {
    if (!REXCVAR_GET(ignore_thread_affinities)) {
      thread_->set_affinity_mask(uint64_t(1) << cpu_index);
    }
  } else {
    REXSYS_WARN("Too few processor cores - scheduling will be wonky");
  }
}

bool XThread::GetTLSValue(uint32_t slot, uint32_t* value_out) {
  if (slot * 4 > tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = memory::load_and_swap<uint32_t>(mem);
  return true;
}

bool XThread::SetTLSValue(uint32_t slot, uint32_t value) {
  if (slot * 4 >= tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  memory::store_and_swap<uint32_t>(mem, value);
  return true;
}

uint32_t XThread::suspend_count() {
  return guest_object<X_KTHREAD>()->suspend_count;
}

X_STATUS XThread::Resume(uint32_t* out_suspend_count) {
  --guest_object<X_KTHREAD>()->suspend_count;

  if (thread_->Resume(out_suspend_count)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_UNSUCCESSFUL;
  }
}

X_STATUS XThread::Suspend(uint32_t* out_suspend_count) {
  auto global_lock = global_critical_region_.Acquire();

  ++guest_object<X_KTHREAD>()->suspend_count;

  // If we are suspending ourselves, we can't hold the lock.
  if (XThread::IsInThread() && XThread::GetCurrentThread() == this) {
    global_lock.unlock();
  }

  if (thread_->Suspend(out_suspend_count)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_UNSUCCESSFUL;
  }
}

X_STATUS XThread::Delay(uint32_t processor_mode, uint32_t alertable, uint64_t interval) {
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
  timeout_ms = chrono::Clock::ScaleGuestDurationMillis(timeout_ms);
  if (alertable) {
    auto result = rex::thread::AlertableSleep(std::chrono::milliseconds(timeout_ms));
    switch (result) {
      default:
      case rex::thread::SleepResult::kSuccess:
        return X_STATUS_SUCCESS;
      case rex::thread::SleepResult::kAlerted:
        return X_STATUS_USER_APC;
    }
  } else {
    rex::thread::Sleep(std::chrono::milliseconds(timeout_ms));
    return X_STATUS_SUCCESS;
  }
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
  state.context.r[14] = ctx->r14.u64;
  state.context.r[15] = ctx->r15.u64;
  state.context.r[16] = ctx->r16.u64;
  state.context.r[17] = ctx->r17.u64;
  state.context.r[18] = ctx->r18.u64;
  state.context.r[19] = ctx->r19.u64;
  state.context.r[20] = ctx->r20.u64;
  state.context.r[21] = ctx->r21.u64;
  state.context.r[22] = ctx->r22.u64;
  state.context.r[23] = ctx->r23.u64;
  state.context.r[24] = ctx->r24.u64;
  state.context.r[25] = ctx->r25.u64;
  state.context.r[26] = ctx->r26.u64;
  state.context.r[27] = ctx->r27.u64;
  state.context.r[28] = ctx->r28.u64;
  state.context.r[29] = ctx->r29.u64;
  state.context.r[30] = ctx->r30.u64;
  state.context.r[31] = ctx->r31.u64;

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
  state.context.f[14] = ctx->f14.f64;
  state.context.f[15] = ctx->f15.f64;
  state.context.f[16] = ctx->f16.f64;
  state.context.f[17] = ctx->f17.f64;
  state.context.f[18] = ctx->f18.f64;
  state.context.f[19] = ctx->f19.f64;
  state.context.f[20] = ctx->f20.f64;
  state.context.f[21] = ctx->f21.f64;
  state.context.f[22] = ctx->f22.f64;
  state.context.f[23] = ctx->f23.f64;
  state.context.f[24] = ctx->f24.f64;
  state.context.f[25] = ctx->f25.f64;
  state.context.f[26] = ctx->f26.f64;
  state.context.f[27] = ctx->f27.f64;
  state.context.f[28] = ctx->f28.f64;
  state.context.f[29] = ctx->f29.f64;
  state.context.f[30] = ctx->f30.f64;
  state.context.f[31] = ctx->f31.f64;

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
  std::memcpy(&state.context.v[14], &ctx->v14, sizeof(vec128_t));
  std::memcpy(&state.context.v[15], &ctx->v15, sizeof(vec128_t));
  std::memcpy(&state.context.v[16], &ctx->v16, sizeof(vec128_t));
  std::memcpy(&state.context.v[17], &ctx->v17, sizeof(vec128_t));
  std::memcpy(&state.context.v[18], &ctx->v18, sizeof(vec128_t));
  std::memcpy(&state.context.v[19], &ctx->v19, sizeof(vec128_t));
  std::memcpy(&state.context.v[20], &ctx->v20, sizeof(vec128_t));
  std::memcpy(&state.context.v[21], &ctx->v21, sizeof(vec128_t));
  std::memcpy(&state.context.v[22], &ctx->v22, sizeof(vec128_t));
  std::memcpy(&state.context.v[23], &ctx->v23, sizeof(vec128_t));
  std::memcpy(&state.context.v[24], &ctx->v24, sizeof(vec128_t));
  std::memcpy(&state.context.v[25], &ctx->v25, sizeof(vec128_t));
  std::memcpy(&state.context.v[26], &ctx->v26, sizeof(vec128_t));
  std::memcpy(&state.context.v[27], &ctx->v27, sizeof(vec128_t));
  std::memcpy(&state.context.v[28], &ctx->v28, sizeof(vec128_t));
  std::memcpy(&state.context.v[29], &ctx->v29, sizeof(vec128_t));
  std::memcpy(&state.context.v[30], &ctx->v30, sizeof(vec128_t));
  std::memcpy(&state.context.v[31], &ctx->v31, sizeof(vec128_t));
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
  std::memcpy(&state.context.v[64], &ctx->v64, sizeof(vec128_t));
  std::memcpy(&state.context.v[65], &ctx->v65, sizeof(vec128_t));
  std::memcpy(&state.context.v[66], &ctx->v66, sizeof(vec128_t));
  std::memcpy(&state.context.v[67], &ctx->v67, sizeof(vec128_t));
  std::memcpy(&state.context.v[68], &ctx->v68, sizeof(vec128_t));
  std::memcpy(&state.context.v[69], &ctx->v69, sizeof(vec128_t));
  std::memcpy(&state.context.v[70], &ctx->v70, sizeof(vec128_t));
  std::memcpy(&state.context.v[71], &ctx->v71, sizeof(vec128_t));
  std::memcpy(&state.context.v[72], &ctx->v72, sizeof(vec128_t));
  std::memcpy(&state.context.v[73], &ctx->v73, sizeof(vec128_t));
  std::memcpy(&state.context.v[74], &ctx->v74, sizeof(vec128_t));
  std::memcpy(&state.context.v[75], &ctx->v75, sizeof(vec128_t));
  std::memcpy(&state.context.v[76], &ctx->v76, sizeof(vec128_t));
  std::memcpy(&state.context.v[77], &ctx->v77, sizeof(vec128_t));
  std::memcpy(&state.context.v[78], &ctx->v78, sizeof(vec128_t));
  std::memcpy(&state.context.v[79], &ctx->v79, sizeof(vec128_t));
  std::memcpy(&state.context.v[80], &ctx->v80, sizeof(vec128_t));
  std::memcpy(&state.context.v[81], &ctx->v81, sizeof(vec128_t));
  std::memcpy(&state.context.v[82], &ctx->v82, sizeof(vec128_t));
  std::memcpy(&state.context.v[83], &ctx->v83, sizeof(vec128_t));
  std::memcpy(&state.context.v[84], &ctx->v84, sizeof(vec128_t));
  std::memcpy(&state.context.v[85], &ctx->v85, sizeof(vec128_t));
  std::memcpy(&state.context.v[86], &ctx->v86, sizeof(vec128_t));
  std::memcpy(&state.context.v[87], &ctx->v87, sizeof(vec128_t));
  std::memcpy(&state.context.v[88], &ctx->v88, sizeof(vec128_t));
  std::memcpy(&state.context.v[89], &ctx->v89, sizeof(vec128_t));
  std::memcpy(&state.context.v[90], &ctx->v90, sizeof(vec128_t));
  std::memcpy(&state.context.v[91], &ctx->v91, sizeof(vec128_t));
  std::memcpy(&state.context.v[92], &ctx->v92, sizeof(vec128_t));
  std::memcpy(&state.context.v[93], &ctx->v93, sizeof(vec128_t));
  std::memcpy(&state.context.v[94], &ctx->v94, sizeof(vec128_t));
  std::memcpy(&state.context.v[95], &ctx->v95, sizeof(vec128_t));
  std::memcpy(&state.context.v[96], &ctx->v96, sizeof(vec128_t));
  std::memcpy(&state.context.v[97], &ctx->v97, sizeof(vec128_t));
  std::memcpy(&state.context.v[98], &ctx->v98, sizeof(vec128_t));
  std::memcpy(&state.context.v[99], &ctx->v99, sizeof(vec128_t));
  std::memcpy(&state.context.v[100], &ctx->v100, sizeof(vec128_t));
  std::memcpy(&state.context.v[101], &ctx->v101, sizeof(vec128_t));
  std::memcpy(&state.context.v[102], &ctx->v102, sizeof(vec128_t));
  std::memcpy(&state.context.v[103], &ctx->v103, sizeof(vec128_t));
  std::memcpy(&state.context.v[104], &ctx->v104, sizeof(vec128_t));
  std::memcpy(&state.context.v[105], &ctx->v105, sizeof(vec128_t));
  std::memcpy(&state.context.v[106], &ctx->v106, sizeof(vec128_t));
  std::memcpy(&state.context.v[107], &ctx->v107, sizeof(vec128_t));
  std::memcpy(&state.context.v[108], &ctx->v108, sizeof(vec128_t));
  std::memcpy(&state.context.v[109], &ctx->v109, sizeof(vec128_t));
  std::memcpy(&state.context.v[110], &ctx->v110, sizeof(vec128_t));
  std::memcpy(&state.context.v[111], &ctx->v111, sizeof(vec128_t));
  std::memcpy(&state.context.v[112], &ctx->v112, sizeof(vec128_t));
  std::memcpy(&state.context.v[113], &ctx->v113, sizeof(vec128_t));
  std::memcpy(&state.context.v[114], &ctx->v114, sizeof(vec128_t));
  std::memcpy(&state.context.v[115], &ctx->v115, sizeof(vec128_t));
  std::memcpy(&state.context.v[116], &ctx->v116, sizeof(vec128_t));
  std::memcpy(&state.context.v[117], &ctx->v117, sizeof(vec128_t));
  std::memcpy(&state.context.v[118], &ctx->v118, sizeof(vec128_t));
  std::memcpy(&state.context.v[119], &ctx->v119, sizeof(vec128_t));
  std::memcpy(&state.context.v[120], &ctx->v120, sizeof(vec128_t));
  std::memcpy(&state.context.v[121], &ctx->v121, sizeof(vec128_t));
  std::memcpy(&state.context.v[122], &ctx->v122, sizeof(vec128_t));
  std::memcpy(&state.context.v[123], &ctx->v123, sizeof(vec128_t));
  std::memcpy(&state.context.v[124], &ctx->v124, sizeof(vec128_t));
  std::memcpy(&state.context.v[125], &ctx->v125, sizeof(vec128_t));
  std::memcpy(&state.context.v[126], &ctx->v126, sizeof(vec128_t));
  std::memcpy(&state.context.v[127], &ctx->v127, sizeof(vec128_t));

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
  ctx->r14.u64 = state.context.r[14];
  ctx->r15.u64 = state.context.r[15];
  ctx->r16.u64 = state.context.r[16];
  ctx->r17.u64 = state.context.r[17];
  ctx->r18.u64 = state.context.r[18];
  ctx->r19.u64 = state.context.r[19];
  ctx->r20.u64 = state.context.r[20];
  ctx->r21.u64 = state.context.r[21];
  ctx->r22.u64 = state.context.r[22];
  ctx->r23.u64 = state.context.r[23];
  ctx->r24.u64 = state.context.r[24];
  ctx->r25.u64 = state.context.r[25];
  ctx->r26.u64 = state.context.r[26];
  ctx->r27.u64 = state.context.r[27];
  ctx->r28.u64 = state.context.r[28];
  ctx->r29.u64 = state.context.r[29];
  ctx->r30.u64 = state.context.r[30];
  ctx->r31.u64 = state.context.r[31];

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
  ctx->f14.f64 = state.context.f[14];
  ctx->f15.f64 = state.context.f[15];
  ctx->f16.f64 = state.context.f[16];
  ctx->f17.f64 = state.context.f[17];
  ctx->f18.f64 = state.context.f[18];
  ctx->f19.f64 = state.context.f[19];
  ctx->f20.f64 = state.context.f[20];
  ctx->f21.f64 = state.context.f[21];
  ctx->f22.f64 = state.context.f[22];
  ctx->f23.f64 = state.context.f[23];
  ctx->f24.f64 = state.context.f[24];
  ctx->f25.f64 = state.context.f[25];
  ctx->f26.f64 = state.context.f[26];
  ctx->f27.f64 = state.context.f[27];
  ctx->f28.f64 = state.context.f[28];
  ctx->f29.f64 = state.context.f[29];
  ctx->f30.f64 = state.context.f[30];
  ctx->f31.f64 = state.context.f[31];

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
  std::memcpy(&ctx->v14, &state.context.v[14], sizeof(vec128_t));
  std::memcpy(&ctx->v15, &state.context.v[15], sizeof(vec128_t));
  std::memcpy(&ctx->v16, &state.context.v[16], sizeof(vec128_t));
  std::memcpy(&ctx->v17, &state.context.v[17], sizeof(vec128_t));
  std::memcpy(&ctx->v18, &state.context.v[18], sizeof(vec128_t));
  std::memcpy(&ctx->v19, &state.context.v[19], sizeof(vec128_t));
  std::memcpy(&ctx->v20, &state.context.v[20], sizeof(vec128_t));
  std::memcpy(&ctx->v21, &state.context.v[21], sizeof(vec128_t));
  std::memcpy(&ctx->v22, &state.context.v[22], sizeof(vec128_t));
  std::memcpy(&ctx->v23, &state.context.v[23], sizeof(vec128_t));
  std::memcpy(&ctx->v24, &state.context.v[24], sizeof(vec128_t));
  std::memcpy(&ctx->v25, &state.context.v[25], sizeof(vec128_t));
  std::memcpy(&ctx->v26, &state.context.v[26], sizeof(vec128_t));
  std::memcpy(&ctx->v27, &state.context.v[27], sizeof(vec128_t));
  std::memcpy(&ctx->v28, &state.context.v[28], sizeof(vec128_t));
  std::memcpy(&ctx->v29, &state.context.v[29], sizeof(vec128_t));
  std::memcpy(&ctx->v30, &state.context.v[30], sizeof(vec128_t));
  std::memcpy(&ctx->v31, &state.context.v[31], sizeof(vec128_t));
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
  std::memcpy(&ctx->v64, &state.context.v[64], sizeof(vec128_t));
  std::memcpy(&ctx->v65, &state.context.v[65], sizeof(vec128_t));
  std::memcpy(&ctx->v66, &state.context.v[66], sizeof(vec128_t));
  std::memcpy(&ctx->v67, &state.context.v[67], sizeof(vec128_t));
  std::memcpy(&ctx->v68, &state.context.v[68], sizeof(vec128_t));
  std::memcpy(&ctx->v69, &state.context.v[69], sizeof(vec128_t));
  std::memcpy(&ctx->v70, &state.context.v[70], sizeof(vec128_t));
  std::memcpy(&ctx->v71, &state.context.v[71], sizeof(vec128_t));
  std::memcpy(&ctx->v72, &state.context.v[72], sizeof(vec128_t));
  std::memcpy(&ctx->v73, &state.context.v[73], sizeof(vec128_t));
  std::memcpy(&ctx->v74, &state.context.v[74], sizeof(vec128_t));
  std::memcpy(&ctx->v75, &state.context.v[75], sizeof(vec128_t));
  std::memcpy(&ctx->v76, &state.context.v[76], sizeof(vec128_t));
  std::memcpy(&ctx->v77, &state.context.v[77], sizeof(vec128_t));
  std::memcpy(&ctx->v78, &state.context.v[78], sizeof(vec128_t));
  std::memcpy(&ctx->v79, &state.context.v[79], sizeof(vec128_t));
  std::memcpy(&ctx->v80, &state.context.v[80], sizeof(vec128_t));
  std::memcpy(&ctx->v81, &state.context.v[81], sizeof(vec128_t));
  std::memcpy(&ctx->v82, &state.context.v[82], sizeof(vec128_t));
  std::memcpy(&ctx->v83, &state.context.v[83], sizeof(vec128_t));
  std::memcpy(&ctx->v84, &state.context.v[84], sizeof(vec128_t));
  std::memcpy(&ctx->v85, &state.context.v[85], sizeof(vec128_t));
  std::memcpy(&ctx->v86, &state.context.v[86], sizeof(vec128_t));
  std::memcpy(&ctx->v87, &state.context.v[87], sizeof(vec128_t));
  std::memcpy(&ctx->v88, &state.context.v[88], sizeof(vec128_t));
  std::memcpy(&ctx->v89, &state.context.v[89], sizeof(vec128_t));
  std::memcpy(&ctx->v90, &state.context.v[90], sizeof(vec128_t));
  std::memcpy(&ctx->v91, &state.context.v[91], sizeof(vec128_t));
  std::memcpy(&ctx->v92, &state.context.v[92], sizeof(vec128_t));
  std::memcpy(&ctx->v93, &state.context.v[93], sizeof(vec128_t));
  std::memcpy(&ctx->v94, &state.context.v[94], sizeof(vec128_t));
  std::memcpy(&ctx->v95, &state.context.v[95], sizeof(vec128_t));
  std::memcpy(&ctx->v96, &state.context.v[96], sizeof(vec128_t));
  std::memcpy(&ctx->v97, &state.context.v[97], sizeof(vec128_t));
  std::memcpy(&ctx->v98, &state.context.v[98], sizeof(vec128_t));
  std::memcpy(&ctx->v99, &state.context.v[99], sizeof(vec128_t));
  std::memcpy(&ctx->v100, &state.context.v[100], sizeof(vec128_t));
  std::memcpy(&ctx->v101, &state.context.v[101], sizeof(vec128_t));
  std::memcpy(&ctx->v102, &state.context.v[102], sizeof(vec128_t));
  std::memcpy(&ctx->v103, &state.context.v[103], sizeof(vec128_t));
  std::memcpy(&ctx->v104, &state.context.v[104], sizeof(vec128_t));
  std::memcpy(&ctx->v105, &state.context.v[105], sizeof(vec128_t));
  std::memcpy(&ctx->v106, &state.context.v[106], sizeof(vec128_t));
  std::memcpy(&ctx->v107, &state.context.v[107], sizeof(vec128_t));
  std::memcpy(&ctx->v108, &state.context.v[108], sizeof(vec128_t));
  std::memcpy(&ctx->v109, &state.context.v[109], sizeof(vec128_t));
  std::memcpy(&ctx->v110, &state.context.v[110], sizeof(vec128_t));
  std::memcpy(&ctx->v111, &state.context.v[111], sizeof(vec128_t));
  std::memcpy(&ctx->v112, &state.context.v[112], sizeof(vec128_t));
  std::memcpy(&ctx->v113, &state.context.v[113], sizeof(vec128_t));
  std::memcpy(&ctx->v114, &state.context.v[114], sizeof(vec128_t));
  std::memcpy(&ctx->v115, &state.context.v[115], sizeof(vec128_t));
  std::memcpy(&ctx->v116, &state.context.v[116], sizeof(vec128_t));
  std::memcpy(&ctx->v117, &state.context.v[117], sizeof(vec128_t));
  std::memcpy(&ctx->v118, &state.context.v[118], sizeof(vec128_t));
  std::memcpy(&ctx->v119, &state.context.v[119], sizeof(vec128_t));
  std::memcpy(&ctx->v120, &state.context.v[120], sizeof(vec128_t));
  std::memcpy(&ctx->v121, &state.context.v[121], sizeof(vec128_t));
  std::memcpy(&ctx->v122, &state.context.v[122], sizeof(vec128_t));
  std::memcpy(&ctx->v123, &state.context.v[123], sizeof(vec128_t));
  std::memcpy(&ctx->v124, &state.context.v[124], sizeof(vec128_t));
  std::memcpy(&ctx->v125, &state.context.v[125], sizeof(vec128_t));
  std::memcpy(&ctx->v126, &state.context.v[126], sizeof(vec128_t));
  std::memcpy(&ctx->v127, &state.context.v[127], sizeof(vec128_t));

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

bool XThread::Save(stream::ByteStream* stream) {
  if (!guest_thread_) {
    // Host XThreads are expected to be recreated on their own.
    return false;
  }

  REXSYS_DEBUG("XThread {:08X} serializing...", handle());

  uint32_t pc = 0;
  if (running_) {
    // TODO(tomc): do we need rexglue-compatible thread serialization?
    //             ideally any previous use for this (multi-dvds) are reworked in recomp to be
    //             single xex
    REXSYS_WARN("XThread {:08X} serialization not implemented", handle());
    return false;
  }

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(kThreadSaveSignature);
  stream->Write(thread_name_);

  ThreadSavedState state;
  state.thread_id = thread_id_;
  state.is_main_thread = main_thread_;
  state.is_running = running_;
  state.apc_head = 0;  // APCs now live in guest-memory typed lists
  state.tls_static_address = tls_static_address_;
  state.tls_dynamic_address = tls_dynamic_address_;
  state.tls_total_size = tls_total_size_;
  state.pcr_address = pcr_address_;
  state.stack_base = stack_base_;
  state.stack_limit = stack_limit_;
  state.stack_alloc_base = stack_alloc_base_;
  state.stack_alloc_size = stack_alloc_size_;

  if (running_) {
    auto context = thread_state_->context();
    SaveContext(context, state);
    state.context.pc = pc;
  }

  stream->Write(&state, sizeof(ThreadSavedState));
  return true;
}

object_ref<XThread> XThread::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  // Kind-of a hack, but we need to set the kernel state outside of the object
  // constructor so it doesn't register a handle with the object table.
  auto thread = new XThread(nullptr);
  thread->kernel_state_ = kernel_state;

  if (!thread->RestoreObject(stream)) {
    return nullptr;
  }

  if (stream->Read<uint32_t>() != kThreadSaveSignature) {
    REXSYS_ERROR("Could not restore XThread - invalid magic!");
    return nullptr;
  }

  REXSYS_DEBUG("XThread {:08X}", thread->handle());

  thread->thread_name_ = stream->Read<std::string>();

  ThreadSavedState state;
  stream->Read(&state, sizeof(ThreadSavedState));
  thread->thread_id_ = state.thread_id;
  thread->main_thread_ = state.is_main_thread;
  thread->running_ = state.is_running;
  // state.apc_head is ignored - APCs live in guest-memory typed lists
  thread->tls_static_address_ = state.tls_static_address;
  thread->tls_dynamic_address_ = state.tls_dynamic_address;
  thread->tls_total_size_ = state.tls_total_size;
  thread->pcr_address_ = state.pcr_address;
  thread->stack_base_ = state.stack_base;
  thread->stack_limit_ = state.stack_limit;
  thread->stack_alloc_base_ = state.stack_alloc_base;
  thread->stack_alloc_size_ = state.stack_alloc_size;

  // Register now that we know our thread ID.
  kernel_state->RegisterThread(thread);

  // Create thread state
  thread->thread_state_ = std::make_unique<runtime::ThreadState>(
      thread->thread_id_, thread->stack_base_, thread->pcr_address_, kernel_state->memory());

  if (state.is_running) {
    auto context = thread->thread_state_->context();
    context->kernel_state = kernel_state;
    LoadContext(context, state);

    // Always retain when starting - the thread owns itself until exited.
    thread->RetainHandle();

    rex::thread::Thread::CreationParameters params;
    params.create_suspended = true;  // Not done restoring yet.
    params.stack_size = 16_MiB;
    thread->thread_ = rex::thread::Thread::Create(params, [thread, state]() {
      // Set thread ID override. This is used by logging.
      rex::thread::set_current_thread_id(thread->handle());

      // Set name immediately, if we have one.
      thread->thread_->set_name(thread->name());

      PROFILE_THREAD_ENTER(thread->name().c_str());

      current_xthread_tls_ = thread;

      // Acquire any mutants
      for (auto mutant : thread->pending_mutant_acquires_) {
        uint64_t timeout = 0;
        auto status = mutant->Wait(0, 0, 0, &timeout);
        assert_true(status == X_STATUS_SUCCESS);
      }
      thread->pending_mutant_acquires_.clear();

      // Execute user code.
      thread->running_ = true;

      // TODO(tomc): do we need this? threads would need different restoration approach
      //             see XThread::Save
      REXSYS_ERROR("Thread restore not implemented");
      (void)state;

      current_xthread_tls_ = nullptr;

      PROFILE_THREAD_EXIT();

      // Release the self-reference to the thread.
      thread->ReleaseHandle();
    });
    assert_not_null(thread->thread_);

    // NOTE(tomc): if this is kept and processor notification dispatch is implemented,
    //             this needs to send a signal to the processor that a thread was started
  }

  return object_ref<XThread>(thread);
}

XHostThread::XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
                         std::function<int()> host_fn)
    : XThread(kernel_state, stack_size, 0, 0, 0, creation_flags, false), host_fn_(host_fn) {
  // NOTE(tomc): there was a start suspended check here before but I don't think we need it.
}

void XHostThread::Execute() {
  REXSYS_INFO("XThread::Execute thid {} (handle={:08X}, '{}', native={:08X}, <host>)", thread_id_,
              handle(), thread_name_, thread_->system_id());

  // Let the kernel know we are starting.
  kernel_state()->OnThreadExecute(this);

  int ret = host_fn_();

  // Exit.
  Exit(ret);
}

}  // namespace rex::system
