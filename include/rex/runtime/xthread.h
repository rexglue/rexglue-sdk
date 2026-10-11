/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/platform/fiber.h>
#include <rex/runtime/pin_count.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/util/native_list.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xsemaphore.h>
#include <rex/runtime/xtimer.h>
#include <rex/runtime/xtypes.h>
#include <rex/platform/thread.h>
#include <rex/types.h>
#include <rex/move_only_function.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** Interrupt request levels stored in `X_KPCR::current_irql`. */
enum IRQL_FLAGS : u8 {
  IRQL_PASSIVE = 0,  /**< Lowest level. */
  IRQL_APC = 1,      /**< Level 1, above passive. */
  IRQL_DISPATCH = 2, /**< Level held while a spinlock is acquired and while queued DPCs run. */
  IRQL_DPC = 3,      /**< Level held while an interrupt routine runs. */
  IRQL_AUDIO = 68,   /**< Level 68. No code uses it. */
  IRQL_CLOCK = 116,  /**< Level 116. No code uses it. */
  IRQL_HIGHEST = 124 /**< Highest level defined. */
};

/** Object types stored in the type byte of `X_DISPATCH_HEADER`. */
enum X_DISPATCHER_FLAGS : u8 {
  DISPATCHER_MANUAL_RESET_EVENT = 0, /**< Notification event. */
  DISPATCHER_AUTO_RESET_EVENT = 1,   /**< Synchronization event. */
  DISPATCHER_MUTANT = 2,             /**< Mutant. */
  DISPATCHER_QUEUE = 4,              /**< Queue. */
  DISPATCHER_SEMAPHORE = 5,          /**< Semaphore. */
  DISPATCHER_THREAD = 6,             /**< Thread. */
  DISPATCHER_MANUAL_RESET_TIMER = 8, /**< Notification timer. */
  DISPATCHER_AUTO_RESET_TIMER = 9,   /**< Synchronization timer. */
};

/** Values of `X_KTHREAD::thread_state`. */
enum X_KTHREAD_STATE_FLAGS : u8 {
  KTHREAD_STATE_INITIALIZED = 0, /**< Written as literal 0 at thread creation. */
  KTHREAD_STATE_READY = 1,       /**< Never written or compared. */
  KTHREAD_STATE_RUNNING = 2,     /**< Value 2. */
  KTHREAD_STATE_STANDBY = 3,     /**< Never written or compared. */
  KTHREAD_STATE_TERMINATED = 4,  /**< Written when the thread exits. */
  KTHREAD_STATE_WAITING = 5,     /**< Never written or compared. */
  KTHREAD_STATE_UNKNOWN = 6,     /**< Never written or compared. */
};

constexpr u32 X_CREATE_SUSPENDED = 0x00000001;

constexpr u32 X_TLS_OUT_OF_INDEXES = UINT32_MAX;

/** Guest layout of a deferred procedure call. */
struct XDPC {
  be_u16 type;             /**< Object type, 19 for a DPC. */
  u8 selected_cpu_number;  /**< 1 while the DPC is queued, else 0. */
  u8 desired_cpu_number;   /**< Cleared by Initialize and read by no code. */
  X_LIST_ENTRY list_entry; /**< Links the DPC in the processor queue. */
  be_u32 routine;          /**< Guest address of the routine. */
  be_u32 context;          /**< First argument of the routine. */
  be_u32 arg1;             /**< Second argument of the routine. */
  be_u32 arg2;             /**< Third argument of the routine. */

  /**
   * Sets the routine and context and clears the queue links and arguments.
   *
   * @param guest_func The guest address of the routine.
   * @param guest_context The context passed to the routine.
   */
  void Initialize(u32 guest_func, u32 guest_context) {
    type = 19;
    selected_cpu_number = 0;
    desired_cpu_number = 0;
    list_entry.flink_ptr = 0;
    list_entry.blink_ptr = 0;
    routine = guest_func;
    context = guest_context;
    arg1 = 0;
    arg2 = 0;
  }
};
REX_STATIC_ASSERT_SIZE(XDPC, 0x1C);

/** A guest XTHREAD_NOTIFICATION. */
struct X_THREAD_NOTIFICATION {
  /** Links the registration in the title's list. */
  X_LIST_ENTRY list_entry;
  /** Called with TRUE when a guest thread starts and FALSE when it exits. */
  be_u32 notify_routine;
};
REX_STATIC_ASSERT_SIZE(X_THREAD_NOTIFICATION, 0xC);

/**
 * Kernel APC stored in guest memory. 40 bytes, 4 shorter than NT as the reserved dword at +4 is
 * absent.
 */
struct XAPC {
  /** Size in bytes of an APC block, allocated for each APC the host queues. */
  static constexpr u32 kSize = 40;
  /** Kernel routine address that marks an APC the host queued itself, never called. */
  static constexpr u32 kDummyKernelRoutine = 0xF00DFF00;
  /** Rundown routine address stored beside kDummyKernelRoutine in an APC the host queued itself. */
  static constexpr u32 kDummyRundownRoutine = 0xF00DFF01;

  u16 type;
  u8 apc_mode;
  u8 enqueued;
  be_u32 thread_ptr;
  X_LIST_ENTRY list_entry;
  be_u32 kernel_routine;
  be_u32 rundown_routine;
  be_u32 normal_routine;
  be_u32 normal_context;
  be_u32 arg1;
  be_u32 arg2;
};
REX_STATIC_ASSERT_SIZE(XAPC, 40);

/** Guest fiber context buffer layout. */
struct X_FIBER_CONTEXT {
  be_u32 fiber_data;       /**< lpParameter. */
  be_u32 stack_alloc_base; /**< KTHREAD.stack_alloc_base (0xD0). */
  /** KTHREAD.stack_base (0x5C), PCR.stack_base_ptr. */
  be_u32 stack_base;
  /** KTHREAD.stack_limit (0x60), PCR.stack_end_ptr. */
  be_u32 stack_limit;
  rex::be_u32 cr_save;  /**< Saved CR. */
  u8 reserved_14[0x08]; /**< Padding. */
  rex::be_u32 lr_save;  /**< Saved LR. */
  u8 reserved_20[0x10]; /**< Padding. */
  be_u64 sp_save;       /**< Saved r1. */
  /** Non volatile PPCContext register save area. */
  u8 register_save_area[0xA50 - 0x38];
};
REX_STATIC_ASSERT_SIZE(X_FIBER_CONTEXT, 0xA50);
static_assert(sizeof(X_FIBER_CONTEXT::register_save_area) >= ::PPCContext::kNonVolatileSaveSize,
              "fiber register save area too small for PPCContext non-volatiles");

struct X_KTHREAD;
struct X_KPROCESS;

/** Guest layout of a wait block, which ties a thread to an object it waits on. */
struct X_KWAIT_BLOCK {
  X_LIST_ENTRY wait_list_entry;
  TypedGuestPointer<X_KTHREAD> thread;
  TypedGuestPointer<X_DISPATCH_HEADER> object;
  TypedGuestPointer<X_KWAIT_BLOCK> next_wait_block;
  be_u16 wait_result_xstatus;
  be_u16 wait_type;
};
REX_STATIC_ASSERT_SIZE(X_KWAIT_BLOCK, 0x18);

/** Guest layout of the processor control block embedded in `X_KPCR`. */
struct X_KPRCB {
  TypedGuestPointer<X_KTHREAD> current_thread; /**< Thread running on this processor. */
  TypedGuestPointer<X_KTHREAD> next_thread;    /**< Thread chosen to run next on this processor. */
  TypedGuestPointer<X_KTHREAD> idle_thread;    /**< Idle thread of this processor. */
  u8 current_cpu;                              /**< Index of this processor. */
  u8 unk_D[3];                                 /**< Padding. */
  be_u32 processor_mask;                       /**< Mask with only the bit of this processor set. */
  be_u32 dpc_clock;                            /**< Ticks this processor spent running DPCs. */
  be_u32 interrupt_clock;        /**< Ticks this processor spent servicing interrupts. */
  be_u32 unk_1C;                 /**< Interrupt count of this processor. */
  be_u32 unk_20;                 /**< Interprocessor interrupt freeze state of this processor. */
  be_u32 ipi_args[3];            /**< Arguments passed to `ipi_function`. */
  be_u32 targeted_ipi_cpus_mask; /**< Mask of the processors an interprocessor interrupt targets. */
  be_u32 ipi_function; /**< Guest address of the function an interprocessor interrupt runs. */
  TypedGuestPointer<X_KPRCB> ipi_initiator_prcb; /**< Control block of the sender. */
  be_u32 unk_3C;                                 /**< Request summary of this processor. */
  be_u32 dpc_related_40;                         /**< Nonzero while a DPC interrupt is requested. */
  be_u32 dpc_lock;                               /**< Lock that guards `queued_dpcs_list_head`. */
  X_LIST_ENTRY queued_dpcs_list_head; /**< Head of the list of DPCs queued on this processor. */
  be_u32 dpc_active;                  /**< Nonzero while this processor runs a DPC. */
  X_KSPINLOCK spin_lock;              /**< Lock that guards the ready lists of this processor. */
  TypedGuestPointer<X_KTHREAD>
      running_idle_thread; /**< Nonzero while the idle thread of this processor runs. */
  X_SINGLE_LIST_ENTRY enqueued_threads_list; /**< Threads waiting to be readied. */
  be_u32 has_ready_thread_by_priority;       /**< Flags each priority that has a ready thread. */
  be_u32 unk_mask_64;                        /**< Mask for `has_ready_thread_by_priority`. */
  X_LIST_ENTRY
  unk_68[32];                            /**< Dispatcher ready list of each of the 32 priorities. */
  XDPC thread_exit_dpc;                  /**< DPC that finishes the exit of a thread. */
  X_LIST_ENTRY terminating_threads_list; /**< Threads that are terminating on this processor. */
  XDPC switch_thread_processor_dpc;      /**< DPC that moves a thread to another processor. */
};

/** Processor Control Region. */
struct X_KPCR {
  be_u32 tls_ptr;
  be_u32 msr_mask;
  union {
    be_u16 software_interrupt_state;
    struct {
      u8 generic_software_interrupt;
      u8 apc_software_interrupt_state;
    };
  };
  be_u16 unk_0A;
  u8 processtype_value_in_dpc;
  u8 timeslice_ended;
  u8 timer_pending;
  u8 unk_0F;
  be_u32 thread_fpu_related;
  be_u32 thread_vmx_related;
  u8 current_irql;
  u8 background_scheduling_active;
  u8 background_scheduling_1A;
  u8 background_scheduling_1B;
  be_u32 timer_related;
  u8 unk_20[0x10];
  be_u64 pcr_ptr;
  union {
    u8 unk_38[8];
    u64 host_stash;
  };
  u8 unk_40[28];
  be_u32 unk_stack_5c;
  u8 unk_60[12];
  be_u32 use_alternative_stack;
  be_u32 stack_base_ptr; /**< Stack base address (high addr). */
  be_u32 stack_end_ptr;  /**< Stack end (low addr). */
  be_u32 alt_stack_base_ptr;
  be_u32 alt_stack_end_ptr;
  be_u32 interrupt_handlers[32];
  X_KPRCB prcb_data;
  TypedGuestPointer<X_KPRCB> prcb;
  u8 unk_2AC[0x2C];
};
REX_STATIC_ASSERT_SIZE(X_KPCR, 0x2D8);

/** Guest layout of a kernel thread object. */
struct X_KTHREAD {
  X_DISPATCH_HEADER header;
  be_u32 unk_10;
  be_u32 unk_14;
  X_KTIMER wait_timeout_timer;
  X_KWAIT_BLOCK wait_timeout_block;
  rex::be_u32 kernel_time;
  be_u32 stack_base;
  be_u32 stack_limit;
  be_u32 stack_kernel;
  be_u32 tls_address;
  u8 thread_state;
  u8 alerted[2];
  u8 alertable;
  u8 priority;
  u8 fpu_exceptions_on;
  u8 process_type_dup; /**< Same as process_type. */
  u8 process_type;
  util::X_TYPED_LIST<XAPC, offsetof(XAPC, list_entry)> apc_lists[2]; /**< Per mode. */
  TypedGuestPointer<X_KPROCESS> process;
  u8 executing_kernel_apc;
  u8 deferred_apc_software_interrupt_state;
  u8 user_apc_pending;
  u8 may_queue_apcs;
  X_KSPINLOCK apc_lock;
  be_u32 num_context_switches_to;
  X_LIST_ENTRY ready_prcb_entry;
  be_u32 msr_mask;
  rex::be<X_STATUS> wait_result;
  u8 wait_irql;
  u8 unk_A5[0xB];
  i32 apc_disable_count;
  be_i32 quantum;
  u8 unk_B8;
  u8 unk_B9;
  u8 unk_BA;
  u8 boost_disabled;
  u8 suspend_count;
  u8 was_preempted;
  u8 terminated;
  u8 current_cpu; /**< Index of this processor. */
  TypedGuestPointer<X_KPRCB> a_prcb_ptr;
  TypedGuestPointer<X_KPRCB> another_prcb_ptr;
  u8 unk_C8;
  u8 unk_C9;
  u8 unk_CA;
  u8 unk_CB;
  X_KSPINLOCK timer_list_lock;
  be_u32 stack_alloc_base;
  XAPC on_suspend;
  X_KSEMAPHORE suspend_sema;
  X_LIST_ENTRY process_threads;
  be_u32 unk_118;
  X_LIST_ENTRY queue_related;
  be_u32 unk_124;
  be_u32 unk_128;
  be_u32 unk_12C;
  be_u64 create_time;
  be_u64 exit_time;
  be_u32 exit_status;
  X_LIST_ENTRY timer_list;
  be_u32 thread_id;
  be_u32 start_address;
  X_LIST_ENTRY unk_154;
  u8 unk_15C[0x4];
  be_u32 last_error;
  be_u32 fiber_ptr;
  u8 unk_168[0x4];
  be_u32 creation_flags;
  u8 unk_170[0xC];
  be_u32 unk_17C;
  u8 unk_180[0x930];
};
REX_STATIC_ASSERT_SIZE(X_KTHREAD, 0xAB0);

class ThreadRegistry;

/** The table slot data of a thread, naming the `ThreadRecord` in the registry by thread id. */
struct ThreadRef : public rex::NonCopyable {
  /**
   * @param thread_id_in The thread id.
   * @param registry_in The registry that owns the `ThreadRecord`.
   */
  ThreadRef(u32 thread_id_in, ThreadRegistry* registry_in)
      : thread_id(thread_id_in), registry(registry_in) {}
  /** Tells the registry the thread lost its handles. */
  REX_RUNTIME_API ~ThreadRef();

  u32 thread_id;            /**< Id of the thread whose `ThreadRecord` the registry holds. */
  ThreadRegistry* registry; /**< Registry told when the thread loses its handles. */
};

/** A guest thread or a host worker, owned by the thread registry. */
class REX_RUNTIME_API ThreadRecord : public rex::NonCopyable {
 public:
  /** Lowest guest address AllocateGuestStack places a stack at. */
  static constexpr u32 kStackAddressRangeBegin = 0x70000000;
  /** Highest guest address AllocateGuestStack places a stack at, exclusive end of the range. */
  static constexpr u32 kStackAddressRangeEnd = 0x7F000000;

  /** Parameters a guest thread was created with. */
  struct CreationParams {
    u32 stack_size;          /**< Guest stack size in bytes, 0 for the process default. */
    u32 xapi_thread_startup; /**< Guest address of the startup trampoline, 0 for a raw thread. */
    u32 start_address;       /**< Guest address of the entry point. */
    u32 start_context;       /**< Argument passed to the entry point. */
    u32 creation_flags;      /**< Creation flag bits, bit 0 is `X_CREATE_SUSPENDED`. */
    u32 guest_process;       /**< Guest address of the process block, 0 for the title process. */
  };

  /**
   * Builds a guest `ThreadRecord`. The host thread starts when the registry launches the
   * `ThreadRecord`.
   *
   * @param runtime The runtime the thread belongs to.
   * @param stack_size The guest stack size, 0 for the process default.
   * @param xapi_thread_startup The startup trampoline, or 0 for a raw thread.
   * @param start_address The entry point.
   * @param start_context The entry point argument.
   * @param creation_flags The creation flags, bit 0 creates the thread suspended.
   * @param guest_thread True for a thread created by the guest.
   * @param main_thread True for the entry point thread.
   * @param guest_process The title process block address, 0 for the title process.
   */
  ThreadRecord(Runtime& runtime, u32 stack_size, u32 xapi_thread_startup, u32 start_address,
               u32 start_context, u32 creation_flags, bool guest_thread, bool main_thread = false,
               u32 guest_process = 0);

  /**
   * Builds a host worker `ThreadRecord`.
   *
   * @param runtime The runtime the thread belongs to.
   * @param stack_size The guest stack size.
   * @param creation_flags The creation flags.
   * @param host_fn The function the worker runs. Its result is the exit code.
   */
  ThreadRecord(Runtime& runtime, u32 stack_size, u32 creation_flags,
               rex::MoveOnlyFunction<int()> host_fn);
  /** Frees the host thread, the scratch block, the stack, the PCR and the kernel thread block. */
  ~ThreadRecord();

  /**
   * Reports whether the calling thread is the given `ThreadRecord`.
   *
   * @param other The `ThreadRecord`.
   * @return True when it is.
   */
  static bool IsInThread(const ThreadRecord* other);

  /**
   * Reports whether the calling thread has a `ThreadRecord`.
   *
   * @return True when it does.
   */
  static bool IsInThread();

  /**
   * Returns the `ThreadRecord` of the calling thread.
   *
   * @return The `ThreadRecord`.
   */
  static ThreadRecord* GetCurrentThread();

  /**
   * Returns the handle of the calling thread.
   *
   * @return The handle.
   */
  static u32 GetCurrentThreadHandle();

  /**
   * Returns the thread id of the calling thread.
   *
   * @return The thread id.
   */
  static u32 GetCurrentThreadId();

  /** Throws rex::ThreadStop when the calling thread has a stop request or the title is terminating.
   */
  static void CheckStop();

  /**
   * Sleeps the calling thread, waking early to stop when it has a stop request.
   *
   * @param duration The time to sleep.
   */
  static void Sleep(std::chrono::microseconds duration);

  /** Writes the CPU time of the calling thread to its KTHREAD. Does nothing without a
   * `ThreadRecord`. */
  static void WriteKernelTime();

  /**
   * Returns the last error of the calling thread.
   *
   * @return The error code.
   */
  static u32 GetLastError();

  /**
   * Sets the last error of the calling thread.
   *
   * @param error_code The error code.
   */
  static void SetLastError(u32 error_code);

  /**
   * Returns the runtime the thread belongs to.
   *
   * @return The runtime.
   */
  Runtime& runtime() const { return *runtime_; }

  /**
   * Returns the guest memory.
   *
   * @return The memory.
   */
  runtime::Memory* memory() const;

  /**
   * Returns the creation handle.
   *
   * @return The handle, 0 before launch.
   */
  X_HANDLE handle() const { return handle_; }

  /**
   * Returns the guest address of the thread block.
   *
   * @return The address.
   */
  u32 guest_object() const { return guest_object_ptr_; }

  /**
   * Returns the thread block as a host pointer.
   *
   * @tparam T The block type.
   * @return The pointer.
   */
  template <typename T>
  T* guest_object() {
    return memory()->TranslateVirtual<T*>(guest_object_ptr_);
  }

  /**
   * Returns the thread name.
   *
   * @return The name.
   */
  const std::string& name() const { return thread_name_; }

  /**
   * Returns the thread name without allocating or locking, for a crash report.
   *
   * @return The name, cut to 63 characters.
   */
  std::string_view crash_name() const {
    return {crash_name_.data(), crash_name_length_.load(std::memory_order_acquire)};
  }

  /**
   * Returns the creation parameters.
   *
   * @return The parameters.
   */
  const CreationParams* creation_params() const { return &creation_params_; }

  /**
   * Returns the guest address of the static TLS block.
   *
   * @return The address.
   */
  u32 tls_ptr() const { return tls_static_address_; }

  /**
   * Returns the guest address of the PCR.
   *
   * @return The address.
   */
  u32 pcr_ptr() const { return pcr_address_; }

  /**
   * Reports whether the title created the thread.
   *
   * @return True when it did.
   */
  bool is_guest_thread() const { return guest_thread_; }

  /**
   * Reports whether this is the entry point thread.
   *
   * @return True when it is.
   */
  bool main_thread() const { return main_thread_; }

  /**
   * Reports whether the thread is running.
   *
   * @return True while it runs.
   */
  bool is_running() const { return running_; }

  /**
   * Returns the thread id.
   *
   * @return The id.
   */
  u32 thread_id() const { return thread_id_; }

  /**
   * Returns the last error.
   *
   * @return The error code.
   */
  u32 last_error();

  /**
   * Sets the last error.
   *
   * @param error_code The error code.
   */
  void set_last_error(u32 error_code);

  /**
   * Sets the thread name.
   *
   * @param name The name, the handle is appended.
   */
  void set_name(const std::string_view name);

  /**
   * Exits the calling thread by unwinding to its entry.
   *
   * @param exit_code The exit code.
   */
  [[noreturn]] void Exit(int exit_code);

  /**
   * Asks the thread to stop at its next wait or kernel call and wakes it.
   *
   * @param exit_code The exit code the thread ends with.
   */
  void RequestStop(i32 exit_code = 0);

  /**
   * Sends the thread to its requested stop when it runs recompiled code.
   *
   * @return True when the thread now unwinds to its entry.
   */
  bool ForceStop();

  /**
   * Returns the main fiber.
   *
   * @return The fiber, null before the thread runs.
   */
  rex::platform::Fiber* main_fiber() const { return main_fiber_; }

  /**
   * Sets the main fiber.
   *
   * @param fiber The fiber.
   */
  void set_main_fiber(rex::platform::Fiber* fiber) { main_fiber_ = fiber; }

  /** Disables APC delivery by 1 level. */
  void EnterCriticalRegion();

  /** Enables APC delivery by 1 level. */
  void LeaveCriticalRegion();

  /** Runs the queued user APCs. */
  void DeliverAPCs();

  /**
   * Queues a user APC.
   *
   * @param normal_routine The routine.
   * @param normal_context The routine context.
   * @param arg1 The first argument.
   * @param arg2 The second argument.
   */
  void EnqueueAPC(u32 normal_routine, u32 normal_context, u32 arg1, u32 arg2);

  /**
   * Returns the stored priority.
   *
   * @return The priority.
   */
  i32 priority() const { return priority_; }

  /**
   * Returns the host priority.
   *
   * @return The priority.
   */
  i32 QueryPriority();

  /**
   * Sets the priority.
   *
   * @param increment The priority increment.
   */
  void SetPriority(i32 increment);

  /**
   * Sets the affinity. Logical processors are 0 core 0 thread 0, 1 core 0 thread 1, 2 core 1
   * thread 0, 3 core 1 thread 1, 4 core 2 thread 0 and 5 core 2 thread 1.
   *
   * @param affinity The processor mask.
   */
  void SetAffinity(u32 affinity);

  /**
   * Returns the logical processor.
   *
   * @return The processor index.
   */
  u8 active_cpu() const;

  /**
   * Moves the thread to a logical processor.
   *
   * @param cpu_index The processor index.
   */
  void SetActiveCPU(u8 cpu_index);

  /**
   * Reads a TLS slot.
   *
   * @param slot The slot.
   * @param value_out Receives the value.
   * @return True when the slot is in range.
   */
  bool GetTLSValue(u32 slot, u32* value_out) const;

  /**
   * Writes a TLS slot.
   *
   * @param slot The slot.
   * @param value The value.
   * @return True when the slot is in range.
   */
  bool SetTLSValue(u32 slot, u32 value);

  /**
   * Returns the suspend count.
   *
   * @return The count.
   */
  u32 suspend_count();

  /**
   * Resumes the thread.
   *
   * @param out_suspend_count Receives the previous count, may be null.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  X_STATUS Resume(u32* out_suspend_count = nullptr);

  /**
   * Suspends the thread.
   *
   * @param out_suspend_count Receives the previous count, may be null.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  X_STATUS Suspend(u32* out_suspend_count = nullptr);

  /**
   * Sleeps the calling thread.
   *
   * @param processor_mode The processor mode.
   * @param alertable True when the sleep ends on a user APC.
   * @param interval The interval in 100 nanosecond ticks, negative for relative.
   * @return X_STATUS_SUCCESS, or X_STATUS_USER_APC.
   */
  X_STATUS Delay(u32 processor_mode, u32 alertable, u64 interval) const;

  /**
   * Returns the host thread.
   *
   * @return The thread.
   */
  rex::platform::Thread* thread() { return thread_.get(); }

  /**
   * Returns the thread state.
   *
   * @return The state.
   */
  runtime::ThreadState* thread_state() { return thread_state_.get(); }

  PinCount pins; /**< Transient pins that keep the object alive while a caller uses it. */

 protected:
  /** Frees the guest stack and clears the TLS slot count and the stack addresses. */
  void FreeStack();
  /** Fills the `X_KTHREAD` block of the thread. */
  void InitializeGuestObject();

  /** Removes every queued APC of the thread and calls its rundown routine. */
  void RundownAPCs();
  /**
   * Calls the registered thread notify routines.
   *
   * @param create True when the thread starts, false when it exits.
   */
  void CallThreadNotifyRoutines(bool create);

  /** Parameters the thread was created with, read when the thread starts. */
  CreationParams creation_params_ = {0, 0, 0, 0, 0, 0};

  u32 thread_id_ = 0; /**< Id the registry assigned to the thread. */
  /** Guest address of the scratch block that carries the arguments of APC kernel routine calls. */
  u32 scratch_address_ = 0;
  u32 scratch_size_ = 0; /**< Size in bytes of the scratch block, 64. */
  /** Guest address of the static TLS block above the stack, stored in the PCR. */
  u32 tls_static_address_ = 0;
  /** Guest address of the dynamic TLS slots, placed after the static TLS data. */
  u32 tls_dynamic_address_ = 0;
  /** Number of 4 byte dynamic TLS slots, read by the TLS accessors for bounds checks. */
  std::atomic<u32> tls_slot_count_{0};
  /** Guest address of the X_KPCR block of the thread, 0 until the thread is created. */
  u32 pcr_address_ = 0;
  /** First byte of the stack allocation including the guard page, released by FreeStack. */
  u32 stack_alloc_base_ = 0;
  /** Highest stack address, where the TLS block starts, stored in the PCR and the X_KTHREAD. */
  u32 stack_base_ = 0;
  u32 stack_limit_ = 0; /**< Lowest usable stack address, stored in the PCR and the X_KTHREAD. */
  bool guest_thread_ = false;        /**< True for a guest thread, false for a host worker. */
  bool main_thread_ = false;         /**< True for the thread that runs the title entry point. */
  std::atomic<bool> running_{false}; /**< True from the start of the thread body until it ends. */
  /** True once the thread began exiting, which makes CheckStop return without throwing. */
  std::atomic<bool> exiting_{false};
  /** Exit code carried by the ThreadStop exception that ends the thread. */
  std::atomic<i32> stop_exit_code_{0};
  /** Guards stop_requested_, thread_ and thread_state_ against a concurrent stop request. */
  std::mutex stop_mutex_;
  /** True once a stop was requested, applied to the thread state when it is created. */
  bool stop_requested_ = false;

  /** Name of the thread with its handle appended, guarded by thread_lock_. */
  std::string thread_name_;
  /** Copy of the name cut to 63 characters, readable by a crash report without locking. */
  std::array<char, 64> crash_name_{};
  std::atomic<u8> crash_name_length_{0}; /**< Number of valid characters in crash_name_. */
  /** Execution state of the thread, created with the stack and the PCR. */
  std::unique_ptr<runtime::ThreadState> thread_state_;

  i32 priority_ = 0; /**< Priority increment last passed to SetPriority. */

  /** Guards the suspend count in the X_KTHREAD and suspend_deferred_. */
  std::mutex suspend_mutex_;
  /** Signalled by Resume to release a thread waiting in a deferred suspend. */
  std::condition_variable suspend_cv_;
  /** True when Suspend could not stop the thread, which suspends itself at its next CheckStop. */
  std::atomic<bool> suspend_deferred_{false};

  std::mutex thread_lock_; /**< Guards thread_name_, crash_name_ and the host thread name. */

  /** Host thread that runs the guest thread, null before the thread starts. */
  std::unique_ptr<rex::platform::Thread> thread_ = nullptr;
  /** Fiber the host thread was converted to, destroyed when the thread exits. */
  rex::platform::Fiber* main_fiber_ = nullptr;

 private:
  friend class ThreadRegistry;

  /**
   * Writes the guest object and creates the host thread without running it.
   *
   * @return X_STATUS_SUCCESS, or the failure status.
   */
  X_STATUS Create();

  /** Runs the host thread unless the title created the thread suspended. */
  void Start();

  /** Ends a created host thread that never ran, and waits until it has ended. */
  void Abandon();
  void SetHandle(X_HANDLE handle) { handle_ = handle; }
  void SetGuestObject(u32 address) { guest_object_ptr_ = address; }
  int Execute();
  void Finish(int exit_code);
  [[noreturn]] static void ThrowStop();
  void ReleaseSelf();

  /**
   * Increments the suspend count and blocks until another thread resumes the caller.
   *
   * @return The previous count.
   */
  u32 SelfSuspend();

  /** Blocks the calling thread until a resume clears the suspend it was given in host code. */
  void TakeDeferredSuspend();

  /**
   * Returns the guest address of the process block the thread belongs to.
   *
   * @return The address, 0 before the runtime has kernel globals.
   */
  u32 ProcessAddress() const;

  /**
   * Writes the exit state to the KTHREAD and unlinks it from its process.
   *
   * @param exit_code The exit status.
   */
  void WriteExitState(u32 exit_code);

  /**
   * Returns the processor of a single bit affinity mask.
   *
   * @param mask The mask, 1 bit below bit 6.
   * @return The processor index.
   */
  static u8 ProcessorFromMask(u8 mask);

  Runtime* runtime_;
  ThreadRegistry* registry_ = nullptr;
  X_HANDLE handle_ = 0;
  u32 guest_object_ptr_ = 0;
  rex::MoveOnlyFunction<int()> host_fn_;
  std::atomic<bool> released_{false};
  std::atomic<bool> abandoned_{false};
  bool exited_ = false;
  bool handles_closed_ = false;
};

}  // namespace rex::runtime
