#pragma once
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

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

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

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

enum IRQL_FLAGS : uint8_t {
  IRQL_PASSIVE = 0,
  IRQL_APC = 1,
  IRQL_DISPATCH = 2,
  IRQL_DPC = 3,
  IRQL_AUDIO = 68,
  IRQL_CLOCK = 116,
  IRQL_HIGHEST = 124
};

enum X_DISPATCHER_FLAGS {
  DISPATCHER_MANUAL_RESET_EVENT = 0,
  DISPATCHER_AUTO_RESET_EVENT = 1,
  DISPATCHER_MUTANT = 2,
  DISPATCHER_QUEUE = 4,
  DISPATCHER_SEMAPHORE = 5,
  DISPATCHER_THREAD = 6,
  DISPATCHER_MANUAL_RESET_TIMER = 8,
  DISPATCHER_AUTO_RESET_TIMER = 9,
};

// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke/kthread_state.htm
enum X_KTHREAD_STATE_FLAGS : uint8_t {
  KTHREAD_STATE_INITIALIZED = 0,
  KTHREAD_STATE_READY = 1,
  KTHREAD_STATE_RUNNING = 2,
  KTHREAD_STATE_STANDBY = 3,
  KTHREAD_STATE_TERMINATED = 4,
  KTHREAD_STATE_WAITING = 5,
  KTHREAD_STATE_UNKNOWN = 6,
};

constexpr uint32_t X_CREATE_SUSPENDED = 0x00000001;

constexpr uint32_t X_TLS_OUT_OF_INDEXES = UINT32_MAX;

struct XDPC {
  rex::be<uint16_t> type;
  uint8_t selected_cpu_number;
  uint8_t desired_cpu_number;
  X_LIST_ENTRY list_entry;
  rex::be<uint32_t> routine;
  rex::be<uint32_t> context;
  rex::be<uint32_t> arg1;
  rex::be<uint32_t> arg2;

  void Initialize(uint32_t guest_func, uint32_t guest_context) {
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
static_assert_size(XDPC, 0x1C);

/** A guest XTHREAD_NOTIFICATION. */
struct X_THREAD_NOTIFICATION {
  /** Links the registration in the title's list. */
  X_LIST_ENTRY list_entry;
  /** Called with TRUE when a guest thread starts and FALSE when it exits. */
  rex::be<u32> notify_routine;
};
static_assert_size(X_THREAD_NOTIFICATION, 0xC);

struct XAPC {
  static constexpr uint32_t kSize = 40;
  static constexpr uint32_t kDummyKernelRoutine = 0xF00DFF00;
  static constexpr uint32_t kDummyRundownRoutine = 0xF00DFF01;

  // KAPC is 0x28(40) bytes? (what's passed to ExAllocatePoolWithTag)
  // This is 4b shorter than NT - looks like the reserved dword at +4 is gone.
  // NOTE: stored in guest memory.
  uint16_t type;                      // +0
  uint8_t apc_mode;                   // +2
  uint8_t enqueued;                   // +3
  rex::be<uint32_t> thread_ptr;       // +4
  X_LIST_ENTRY list_entry;            // +8
  rex::be<uint32_t> kernel_routine;   // +16
  rex::be<uint32_t> rundown_routine;  // +20
  rex::be<uint32_t> normal_routine;   // +24
  rex::be<uint32_t> normal_context;   // +28
  rex::be<uint32_t> arg1;             // +32
  rex::be<uint32_t> arg2;             // +36
};
static_assert_size(XAPC, 40);

/// Guest fiber context buffer layout.
struct X_FIBER_CONTEXT {
  rex::be<uint32_t> fiber_data;              // 0x00  lpParameter
  rex::be<uint32_t> stack_alloc_base;        // 0x04  KTHREAD.stack_alloc_base (0xD0)
  rex::be<uint32_t> stack_base;              // 0x08  KTHREAD.stack_base (0x5C), PCR.stack_base_ptr
  rex::be<uint32_t> stack_limit;             // 0x0C  KTHREAD.stack_limit (0x60), PCR.stack_end_ptr
  rex::be_u32 cr_save;                       // 0x10  saved CR
  uint8_t reserved_14[0x08];                 // 0x14  padding
  rex::be_u32 lr_save;                       // 0x1C  saved LR
  uint8_t reserved_20[0x10];                 // 0x20  padding
  rex::be<uint64_t> sp_save;                 // 0x30  saved r1
  uint8_t register_save_area[0xA50 - 0x38];  // non-volatile PPCContext register save area
};
static_assert_size(X_FIBER_CONTEXT, 0xA50);
static_assert(sizeof(X_FIBER_CONTEXT::register_save_area) >= ::PPCContext::kNonVolatileSaveSize,
              "fiber register save area too small for PPCContext non-volatiles");

struct X_KTHREAD;
struct X_KPROCESS;

struct X_KWAIT_BLOCK {
  X_LIST_ENTRY wait_list_entry;                      // 0x0
  TypedGuestPointer<X_KTHREAD> thread;               // 0x8
  TypedGuestPointer<X_DISPATCH_HEADER> object;       // 0xC
  TypedGuestPointer<X_KWAIT_BLOCK> next_wait_block;  // 0x10
  rex::be<uint16_t> wait_result_xstatus;             // 0x14
  rex::be<uint16_t> wait_type;                       // 0x16
};
static_assert_size(X_KWAIT_BLOCK, 0x18);

struct X_KPRCB {
  TypedGuestPointer<X_KTHREAD> current_thread;       // 0x0
  TypedGuestPointer<X_KTHREAD> next_thread;          // 0x4
  TypedGuestPointer<X_KTHREAD> idle_thread;          // 0x8
  uint8_t current_cpu;                               // 0xC
  uint8_t unk_D[3];                                  // 0xD
  rex::be<uint32_t> processor_mask;                  // 0x10
  rex::be<uint32_t> dpc_clock;                       // 0x14
  rex::be<uint32_t> interrupt_clock;                 // 0x18
  rex::be<uint32_t> unk_1C;                          // 0x1C
  rex::be<uint32_t> unk_20;                          // 0x20
  rex::be<uint32_t> ipi_args[3];                     // 0x24
  rex::be<uint32_t> targeted_ipi_cpus_mask;          // 0x30
  rex::be<uint32_t> ipi_function;                    // 0x34
  TypedGuestPointer<X_KPRCB> ipi_initiator_prcb;     // 0x38
  rex::be<uint32_t> unk_3C;                          // 0x3C
  rex::be<uint32_t> dpc_related_40;                  // 0x40
  rex::be<uint32_t> dpc_lock;                        // 0x44
  X_LIST_ENTRY queued_dpcs_list_head;                // 0x48
  rex::be<uint32_t> dpc_active;                      // 0x50
  X_KSPINLOCK spin_lock;                             // 0x54
  TypedGuestPointer<X_KTHREAD> running_idle_thread;  // 0x58
  X_SINGLE_LIST_ENTRY enqueued_threads_list;         // 0x5C
  rex::be<uint32_t> has_ready_thread_by_priority;    // 0x60
  rex::be<uint32_t> unk_mask_64;                     // 0x64
  X_LIST_ENTRY unk_68[32];                           // 0x68
  XDPC thread_exit_dpc;                              // 0x168
  X_LIST_ENTRY terminating_threads_list;             // 0x184
  XDPC switch_thread_processor_dpc;                  // 0x18C
};

// Processor Control Region
struct X_KPCR {
  rex::be<uint32_t> tls_ptr;   // 0x0
  rex::be<uint32_t> msr_mask;  // 0x4
  union {
    rex::be<uint16_t> software_interrupt_state;  // 0x8
    struct {
      uint8_t generic_software_interrupt;    // 0x8
      uint8_t apc_software_interrupt_state;  // 0x9
    };
  };
  rex::be<uint16_t> unk_0A;              // 0xA
  uint8_t processtype_value_in_dpc;      // 0xC
  uint8_t timeslice_ended;               // 0xD
  uint8_t timer_pending;                 // 0xE
  uint8_t unk_0F;                        // 0xF
  rex::be<uint32_t> thread_fpu_related;  // 0x10
  rex::be<uint32_t> thread_vmx_related;  // 0x14
  uint8_t current_irql;                  // 0x18
  uint8_t background_scheduling_active;  // 0x19
  uint8_t background_scheduling_1A;      // 0x1A
  uint8_t background_scheduling_1B;      // 0x1B
  rex::be<uint32_t> timer_related;       // 0x1C
  uint8_t unk_20[0x10];                  // 0x20
  rex::be<uint64_t> pcr_ptr;             // 0x30
  union {
    uint8_t unk_38[8];    // 0x38
    uint64_t host_stash;  // 0x38
  };
  uint8_t unk_40[28];                        // 0x40
  rex::be<uint32_t> unk_stack_5c;            // 0x5C
  uint8_t unk_60[12];                        // 0x60
  rex::be<uint32_t> use_alternative_stack;   // 0x6C
  rex::be<uint32_t> stack_base_ptr;          // 0x70 Stack base address (high addr)
  rex::be<uint32_t> stack_end_ptr;           // 0x74 Stack end (low addr)
  rex::be<uint32_t> alt_stack_base_ptr;      // 0x78
  rex::be<uint32_t> alt_stack_end_ptr;       // 0x7C
  rex::be<uint32_t> interrupt_handlers[32];  // 0x80
  X_KPRCB prcb_data;                         // 0x100
  TypedGuestPointer<X_KPRCB> prcb;           // 0x2A8
  uint8_t unk_2AC[0x2C];                     // 0x2AC
};

struct X_KTHREAD {
  X_DISPATCH_HEADER header;          // 0x0
  rex::be<uint32_t> unk_10;          // 0x10
  rex::be<uint32_t> unk_14;          // 0x14
  X_KTIMER wait_timeout_timer;       // 0x18
  X_KWAIT_BLOCK wait_timeout_block;  // 0x40
  rex::be_u32 kernel_time;           // 0x58
  rex::be<uint32_t> stack_base;      // 0x5C
  rex::be<uint32_t> stack_limit;     // 0x60
  rex::be<uint32_t> stack_kernel;    // 0x64
  rex::be<uint32_t> tls_address;     // 0x68
  uint8_t thread_state;              // 0x6C
  uint8_t alerted[2];                // 0x6D
  uint8_t alertable;                 // 0x6F
  uint8_t priority;                  // 0x70
  uint8_t fpu_exceptions_on;         // 0x71
  uint8_t process_type_dup;          // 0x72 (same as process_type below)
  uint8_t process_type;              // 0x73 (referenced most frequently)
  // apc_mode determines which list an apc goes into
  util::X_TYPED_LIST<XAPC, offsetof(XAPC, list_entry)> apc_lists[2];  // 0x74
  TypedGuestPointer<X_KPROCESS> process;                              // 0x84
  uint8_t executing_kernel_apc;                                       // 0x88
  uint8_t deferred_apc_software_interrupt_state;                      // 0x89
  uint8_t user_apc_pending;                                           // 0x8A
  uint8_t may_queue_apcs;                                             // 0x8B
  X_KSPINLOCK apc_lock;                                               // 0x8C
  rex::be<uint32_t> num_context_switches_to;                          // 0x90
  X_LIST_ENTRY ready_prcb_entry;                                      // 0x94
  rex::be<uint32_t> msr_mask;                                         // 0x9C
  rex::be<X_STATUS> wait_result;                                      // 0xA0
  uint8_t wait_irql;                                                  // 0xA4
  uint8_t unk_A5[0xB];                                                // 0xA5
  int32_t apc_disable_count;                                          // 0xB0
  rex::be<int32_t> quantum;                                           // 0xB4
  uint8_t unk_B8;                                                     // 0xB8
  uint8_t unk_B9;                                                     // 0xB9
  uint8_t unk_BA;                                                     // 0xBA
  uint8_t boost_disabled;                                             // 0xBB
  uint8_t suspend_count;                                              // 0xBC
  uint8_t was_preempted;                                              // 0xBD
  uint8_t terminated;                                                 // 0xBE
  uint8_t current_cpu;                                                // 0xBF
  TypedGuestPointer<X_KPRCB> a_prcb_ptr;                              // 0xC0
  TypedGuestPointer<X_KPRCB> another_prcb_ptr;                        // 0xC4
  uint8_t unk_C8;                                                     // 0xC8
  uint8_t unk_C9;                                                     // 0xC9
  uint8_t unk_CA;                                                     // 0xCA
  uint8_t unk_CB;                                                     // 0xCB
  X_KSPINLOCK timer_list_lock;                                        // 0xCC
  rex::be<uint32_t> stack_alloc_base;                                 // 0xD0
  XAPC on_suspend;                                                    // 0xD4
  X_KSEMAPHORE suspend_sema;                                          // 0xFC
  X_LIST_ENTRY process_threads;                                       // 0x110
  rex::be<uint32_t> unk_118;                                          // 0x118
  X_LIST_ENTRY queue_related;                                         // 0x11C
  rex::be<uint32_t> unk_124;                                          // 0x124
  rex::be<uint32_t> unk_128;                                          // 0x128
  rex::be<uint32_t> unk_12C;                                          // 0x12C
  rex::be<uint64_t> create_time;                                      // 0x130
  rex::be<uint64_t> exit_time;                                        // 0x138
  rex::be<uint32_t> exit_status;                                      // 0x140
  X_LIST_ENTRY timer_list;                                            // 0x144
  rex::be<uint32_t> thread_id;                                        // 0x14C
  rex::be<uint32_t> start_address;                                    // 0x150
  X_LIST_ENTRY unk_154;                                               // 0x154
  uint8_t unk_15C[0x4];                                               // 0x15C
  rex::be<uint32_t> last_error;                                       // 0x160
  rex::be<uint32_t> fiber_ptr;                                        // 0x164
  uint8_t unk_168[0x4];                                               // 0x168
  rex::be<uint32_t> creation_flags;                                   // 0x16C
  uint8_t unk_170[0xC];                                               // 0x170
  rex::be<uint32_t> unk_17C;                                          // 0x17C
  uint8_t unk_180[0x930];                                             // 0x180
};
static_assert_size(X_KTHREAD, 0xAB0);

class ThreadRegistry;

/** The table slot data of a thread, naming the record in the registry by thread id. */
struct ThreadRef {
  /**
   * @param thread_id_in The thread id.
   * @param registry_in The registry that owns the record.
   */
  ThreadRef(u32 thread_id_in, ThreadRegistry* registry_in)
      : thread_id(thread_id_in), registry(registry_in) {}
  ThreadRef(const ThreadRef&) = delete;
  ThreadRef& operator=(const ThreadRef&) = delete;
  ~ThreadRef();

  u32 thread_id;
  ThreadRegistry* registry;
};

/** A guest thread or a host worker, owned by the thread registry. */
class ThreadRecord {
 public:
  static constexpr u32 kStackAddressRangeBegin = 0x70000000;
  static constexpr u32 kStackAddressRangeEnd = 0x7F000000;

  struct CreationParams {
    u32 stack_size;
    u32 xapi_thread_startup;
    u32 start_address;
    u32 start_context;
    u32 creation_flags;
    u32 guest_process;
  };

  /**
   * Builds a guest thread record. The host thread starts when the registry launches the record.
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
   * Builds a host worker record.
   *
   * @param runtime The runtime the thread belongs to.
   * @param stack_size The guest stack size.
   * @param creation_flags The creation flags.
   * @param host_fn The function the worker runs. Its result is the exit code.
   */
  ThreadRecord(Runtime& runtime, u32 stack_size, u32 creation_flags, std::function<int()> host_fn);
  ~ThreadRecord();
  ThreadRecord(const ThreadRecord&) = delete;
  ThreadRecord& operator=(const ThreadRecord&) = delete;

  /**
   * Reports whether the calling thread is the given record.
   *
   * @param other The record.
   * @return True when it is.
   */
  static bool IsInThread(const ThreadRecord* other);

  /**
   * Reports whether the calling thread has a record.
   *
   * @return True when it does.
   */
  static bool IsInThread();

  /**
   * Returns the record of the calling thread.
   *
   * @return The record.
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

  /** Exits the calling thread when the title is terminating. Does not return then. */
  static void CheckTitleTermination();

  /** Writes the CPU time of the calling thread to its KTHREAD. Does nothing without a record. */
  static void NoteKernelTime();

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
   * Exits the calling thread. Does not return.
   *
   * @param exit_code The exit code.
   * @return Never returns on success.
   */
  X_STATUS Exit(int exit_code);

  /**
   * Terminates the thread.
   *
   * @param exit_code The exit code.
   * @return X_STATUS_SUCCESS.
   */
  X_STATUS Terminate(int exit_code);

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

  /** Takes the APC spin lock of the thread. */
  void LockApc();

  /**
   * Releases the APC spin lock of the thread.
   *
   * @param queue_delivery True to wake the thread when APCs are queued.
   */
  void UnlockApc(bool queue_delivery);

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
   * Queries the host priority.
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
  X_STATUS Delay(u32 processor_mode, u32 alertable, u64 interval);

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

  PinCount pins;

 protected:
  void FreeStack();
  void InitializeGuestObject();

  void RundownAPCs();
  void CallThreadNotifyRoutines(bool create);

  CreationParams creation_params_ = {0, 0, 0, 0, 0, 0};

  u32 thread_id_ = 0;
  u32 scratch_address_ = 0;
  u32 scratch_size_ = 0;
  u32 tls_static_address_ = 0;
  u32 tls_dynamic_address_ = 0;
  std::atomic<u32> tls_slot_count_{0};
  u32 pcr_address_ = 0;
  u32 stack_alloc_base_ = 0;
  u32 stack_base_ = 0;
  u32 stack_limit_ = 0;
  bool guest_thread_ = false;
  bool main_thread_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> exit_claimed_{false};
  bool exiting_ = false;

  std::string thread_name_;
  std::unique_ptr<runtime::ThreadState> thread_state_;

  i32 priority_ = 0;

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  std::mutex suspend_mutex_;
  std::condition_variable suspend_cv_;
#endif

  std::mutex thread_lock_;

  u32 apc_lock_old_irql_ = 0;

  std::unique_ptr<rex::platform::Thread> thread_ = nullptr;
  rex::platform::Fiber* main_fiber_ = nullptr;

 private:
  friend class ThreadRegistry;
  X_STATUS Create();
  void SetHandle(X_HANDLE handle) { handle_ = handle; }
  void SetGuestObject(u32 address) { guest_object_ptr_ = address; }
  void Execute();
  void ReleaseSelf();
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

  /**
   * Increments the suspend count and blocks until another thread resumes the caller.
   *
   * @return The previous count.
   */
  u32 SelfSuspend();
#endif

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
  std::function<int()> host_fn_;
  std::unique_ptr<rex::platform::Event> exit_gate_;
  std::atomic<bool> released_{false};
  std::atomic<bool> started_{false};
  bool exited_ = false;
  bool handles_closed_ = false;
};

}  // namespace rex::runtime
