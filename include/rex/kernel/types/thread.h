/**
 * @file        kernel/types/thread.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia (Ben Vanik, 2020).
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/assert.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/types/memory.h>
#include <rex/ppc/context.h>
#include <rex/types.h>
#include <rex/memory.h>

namespace rex::kernel {

/** Interrupt request levels stored in `X_KPCR::current_irql`. */
enum IRQL_FLAGS : u8 {
  kIRQLPassive = 0,
  kIRQLAPC = 1,
  kIRQLDispatch = 2, /**< Level held while a spinlock is acquired and while queued DPCs run. */
  kIRQLDPC = 3,      /**< Level held while an interrupt routine runs. */
  kIRQLAudio = 68,   /**< No code uses it. */
  kIRQLClock = 116,  /**< No code uses it. */
  kIRQLHighest = 124
};

/** Object types stored in the type byte of `X_DISPATCH_HEADER`. */
enum X_DISPATCHER_FLAGS : u8 {
  kDispatcherManualResetEvent = 0,
  kDispatcherAutoResetEvent = 1,
  kDispatcherMutant = 2,
  kDispatcherQueue = 4,
  kDispatcherSemaphore = 5,
  kDispatcherThread = 6,
  kDispatcherManualResetTimer = 8,
  kDispatcherAutoResetTimer = 9,
};

/** Values of `X_KTHREAD::thread_state`. */
enum X_KTHREAD_STATE_FLAGS : u8 {
  kKThreadStateInitialized = 0, /**< Written as literal 0 at thread creation. */
  kKThreadStateReady = 1,       /**< Never written or compared. */
  kKThreadStateRunning = 2,
  kKThreadStateStandby = 3,    /**< Never written or compared. */
  kKThreadStateTerminated = 4, /**< Written when the thread exits. */
  kKThreadStateWaiting = 5,    /**< Never written or compared. */
  kKThreadStateUnknown = 6,    /**< Never written or compared. */
};

constexpr u32 kXCreateSuspended = 0x00000001;

constexpr u32 kXTLSOutOfIndexes = UINT32_MAX; /**< Index returned when no TLS slot is free. */

/** Header at the start of every waitable kernel object. */
struct X_DISPATCH_HEADER {
  struct {
    u8 type; /**< Object type, one of `X_DISPATCHER_FLAGS`. */

    union {
      /** Shares its byte with absolute, npx_irql and signalling. */
      u8 abandoned;
      /** Holds the spin count divided by 256 in a critical section. */
      u8 absolute;
      u8 npx_irql;   /**< No code reads it. */
      u8 signalling; /**< No code reads it. */
    };
    union {
      u8 size; /**< Shares its byte with hand and process_type. */
      u8 hand; /**< No code reads it. */
      /** Process type a timer was initialized with by KeInitializeTimerEx. */
      u8 process_type;
    };
    union {
      u8 inserted;     /**< Shares its byte with debug_active and dpc_active. */
      u8 debug_active; /**< No code reads it. */
      u8 dpc_active;   /**< No code reads it. */
    };
  };

  be_u32 signal_state; /**< Signal count of the object. */
  be_u32 wait_list_flink;
  be_u32 wait_list_blink;
};
REX_STATIC_ASSERT_SIZE(X_DISPATCH_HEADER, 0x10);

struct X_KSEMAPHORE {
  X_DISPATCH_HEADER header;
  be_u32 limit; /**< Largest count the semaphore reaches. */
};
REX_STATIC_ASSERT_SIZE(X_KSEMAPHORE, 0x14);

struct X_KDPC;

struct X_KTIMER {
  X_DISPATCH_HEADER header;
  be_u64 due_time;                 /**< Due time in 100 nanosecond ticks. */
  X_LIST_ENTRY table_bucket_entry; /**< Reserved. */
  be_ptr<X_KDPC> dpc;              /**< The DPC queued when the timer fires, null for none. */
  be_u32 period;                   /**< Repeat period in milliseconds, 0 for a one shot timer. */
};
REX_STATIC_ASSERT_SIZE(X_KTIMER, 0x28);

/** The object body follows this header. */
struct X_OBJECT_HEADER {
  be_u32 pointer_count;   /**< Reference count of the object, zeroed when the block is allocated. */
  be_u32 handle_count;    /**< Handle count of the object, zeroed when the block is allocated. */
  be_u32 object_type_ptr; /**< Guest address of the X_OBJECT_TYPE of the object. */
  be_u16 flags;           /**< Zero, no object flag is set. */
  u8 hash_index;          /**< Zero when the block is allocated. */
  u8 pad_15;
};
static_assert(offsetof(X_OBJECT_HEADER, object_type_ptr) == 0x8);
static_assert(offsetof(X_OBJECT_HEADER, flags) == 0xC);
REX_STATIC_ASSERT_SIZE(X_OBJECT_HEADER, 0x10);

struct X_OBJECT_TYPE {
  be_u32 constructor;
  be_u32 destructor;
  be_u32 unk_08;
  be_u32 unk_0c;
  be_u32 unk_10;
  be_u32 unk_14;
  be_u32 pool_tag; /**< 4 character pool tag of the object type. */
};
REX_STATIC_ASSERT_SIZE(X_OBJECT_TYPE, 0x1C);

struct X_KDPC {
  be_u16 type;             /**< Object type, 19 for a DPC. */
  u8 inserted_number;      /**< 1 while the DPC is queued, else 0. */
  u8 target_number;        /**< Cleared by Initialize and read by no code. */
  X_LIST_ENTRY list_entry; /**< Links the DPC in the processor queue. */
  be_u32 routine;
  be_u32 context; /**< First argument of the routine. */
  be_u32 arg1;    /**< Second argument of the routine. */
  be_u32 arg2;    /**< Third argument of the routine. */

  /** Sets the routine and context and clears the queue links and arguments. */
  void Initialize(u32 guest_func, u32 guest_context) {
    type = 19;
    inserted_number = 0;
    target_number = 0;
    list_entry.flink_ptr = 0;
    list_entry.blink_ptr = 0;
    routine = guest_func;
    context = guest_context;
    arg1 = 0;
    arg2 = 0;
  }
};
static_assert(offsetof(X_KDPC, inserted_number) == 0x2);
static_assert(offsetof(X_KDPC, target_number) == 0x3);
REX_STATIC_ASSERT_SIZE(X_KDPC, 0x1C);

struct X_THREAD_NOTIFICATION {
  /** Links the registration in the title's list. */
  X_LIST_ENTRY list_entry;
  /** Called with TRUE when a guest thread starts and FALSE when it exits. */
  be_u32 notify_routine;
};
REX_STATIC_ASSERT_SIZE(X_THREAD_NOTIFICATION, 0xC);

/**
 * Kernel APC in guest memory, 40 bytes. NT has a reserved dword at +4 that this layout omits.
 */
struct XAPC {
  /** Size in bytes of an APC block, allocated for each APC the host queues. */
  static constexpr u32 kSize = 40;
  /** Kernel routine address that marks an APC the host queued itself, never called. */
  static constexpr u32 kDummyKernelRoutine = 0xF00DFF00;
  /** Rundown routine address stored beside kDummyKernelRoutine in an APC the host queued itself. */
  static constexpr u32 kDummyRundownRoutine = 0xF00DFF01;

  u16 type;    /**< Object type, 18 for an APC. */
  u8 apc_mode; /**< Index into `X_KTHREAD::apc_lists`, 0 for kernel and 1 for user. */
  u8 enqueued; /**< 1 while the APC is in a list of the thread. */
  be_u32 thread_ptr;
  X_LIST_ENTRY list_entry; /**< Links the APC in a list of the thread. */
  be_u32 kernel_routine;
  be_u32 rundown_routine;
  be_u32 normal_routine; /**< Guest address of the normal routine, 0 for a kernel only APC. */
  be_u32 normal_context; /**< First argument of the normal routine. */
  be_u32 arg1;           /**< Second argument of the normal routine, set when queued. */
  be_u32 arg2;           /**< Third argument of the normal routine, set when queued. */
};
REX_STATIC_ASSERT_SIZE(XAPC, 40);

struct X_FIBER_CONTEXT {
  be_u32 fiber_data;       /**< lpParameter. */
  be_u32 stack_alloc_base; /**< KTHREAD.stack_alloc_base. */
  /** KTHREAD.stack_base, PCR.stack_base_ptr. */
  be_u32 stack_base;
  /** KTHREAD.stack_limit, PCR.stack_end_ptr. */
  be_u32 stack_limit;
  rex::be_u32 cr_save;
  u8 reserved_14[0x08];
  rex::be_u32 lr_save;
  u8 reserved_20[0x10];
  be_u64 sp_save; /**< Saved r1. */
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
  X_LIST_ENTRY wait_list_entry; /**< Links the block in the wait list of the object. */
  be_ptr<X_KTHREAD> thread;
  be_ptr<X_DISPATCH_HEADER> object;      /**< Dispatch header of the object waited on. */
  be_ptr<X_KWAIT_BLOCK> next_wait_block; /**< Next block of the same wait. */
  be_u16 wait_result_xstatus;            /**< Wait result value, 0x0100 in the timeout block. */
  be_u16 wait_type;                      /**< Wait type, 0x0201 in the timeout block. */
};
REX_STATIC_ASSERT_SIZE(X_KWAIT_BLOCK, 0x18);

/** Guest layout of the processor control block embedded in `X_KPCR`. */
struct X_KPRCB {
  be_ptr<X_KTHREAD> current_thread;
  be_ptr<X_KTHREAD> next_thread;
  be_ptr<X_KTHREAD> idle_thread;
  u8 current_cpu;
  u8 pad_13[3];
  be_u32 processor_mask;  /**< Mask with only the bit of this processor set. */
  be_u32 dpc_clock;       /**< Ticks this processor spent running DPCs. */
  be_u32 interrupt_clock; /**< Ticks this processor spent servicing interrupts. */
  be_u32 interrupt_count;
  be_u32 ipi_frozen;
  be_u32 ipi_args[3];            /**< Arguments passed to `ipi_function`. */
  be_u32 targeted_ipi_cpus_mask; /**< Mask of the processors an interprocessor interrupt targets. */
  be_u32 ipi_function; /**< Guest address of the function an interprocessor interrupt runs. */
  be_ptr<X_KPRCB> ipi_initiator_prcb; /**< Control block of the sender. */
  be_u32 request_summary;
  be_u32 dpc_interrupt_requested; /**< Nonzero while a DPC interrupt is requested. */
  be_u32 dpc_lock;                /**< Lock that guards `queued_dpcs_list_head`. */
  X_LIST_ENTRY queued_dpcs_list_head;
  be_u32 dpc_active;           /**< Nonzero while this processor runs a DPC. */
  X_KSPINLOCK ready_list_lock; /**< Lock that guards the ready lists of this processor. */
  be_u32 idle_thread_active;   /**< Nonzero while the idle thread of this processor runs. */
  X_SINGLE_LIST_ENTRY enqueued_threads_list;   /**< Threads waiting to be readied. */
  be_u32 has_ready_thread_by_priority;         /**< Flags each priority that has a ready thread. */
  be_u32 ready_summary_mask;                   /**< Mask for `has_ready_thread_by_priority`. */
  X_LIST_ENTRY dispatcher_ready_list_head[32]; /**< Dispatcher ready list per priority. */
  X_KDPC thread_exit_dpc;
  X_LIST_ENTRY terminating_threads_list;
  X_KDPC switch_thread_processor_dpc;
};
REX_STATIC_ASSERT_SIZE(X_KPRCB, 0x1A8);

/** Processor Control Region. */
struct X_KPCR {
  be_u32 tls_ptr; /**< Guest address of the static TLS data of the running thread. */
  /** Reserved. @{ */
  be_u32 msr_mask;
  union {
    be_u16 software_interrupt_state; /**< Reserved. */
    struct {
      u8 dispatch_software_interrupt;  /**< Reserved. */
      u8 apc_software_interrupt_state; /**< Reserved. */
    };
  };
  be_u16 unk_0a;
  /** @} */
  u8 processtype_value_in_dpc; /**< Process type reported while a DPC runs. */
  /** Reserved. @{ */
  u8 timeslice_ended;
  u8 timer_pending;
  u8 unk_0f;
  be_u32 thread_fpu_related;
  be_u32 thread_vmx_related;
  /** @} */
  u8 current_irql; /**< Current interrupt request level, one of the `IRQL_FLAGS` values. */
  /** Reserved. @{ */
  u8 background_scheduling_active;
  u8 background_scheduling_1a;
  u8 background_scheduling_1b;
  be_u32 timer_related;
  u8 unk_20[0x10];
  /** @} */
  be_u64 pcr_ptr; /**< Guest address of this region. */
  union {
    u8 unk_38[8];   /**< Reserved, shares its bytes with host_stash. */
    u64 host_stash; /**< Host address of the register file of the running thread. */
  };
  /** Reserved. @{ */
  u8 unk_40[28];
  be_u32 unk_stack_5c;
  u8 unk_60[12];
  be_u32 use_alternative_stack;
  /** @} */
  be_u32 stack_base_ptr; /**< Guest address of the high end of the stack. */
  be_u32 stack_end_ptr;  /**< Guest address of the low end of the stack. */
  /** Reserved. @{ */
  be_u32 alt_stack_base_ptr;
  be_u32 alt_stack_end_ptr;
  be_u32 interrupt_handlers[32];
  /** @} */
  X_KPRCB prcb_data;
  be_ptr<X_KPRCB> prcb; /**< Guest address of prcb_data. */
  u8 unk_2ac[0x2C];     /**< Reserved. */
};
static_assert(offsetof(X_KPCR, prcb_data) == 0x100);
REX_STATIC_ASSERT_SIZE(X_KPCR, 0x2D8);

struct X_KTHREAD {
  X_DISPATCH_HEADER header;
  /** Part of an empty list head that points at itself. @{ */
  be_u32 unk_10;
  be_u32 unk_14;
  /** @} */
  X_KTIMER wait_timeout_timer;      /**< Reserved. */
  X_KWAIT_BLOCK wait_timeout_block; /**< Wait block set at creation for the timeout of a wait. */
  rex::be_u32 kernel_time;          /**< CPU time in milliseconds when last written. */
  be_u32 stack_base;                /**< Guest address of the high end of the stack. */
  be_u32 stack_limit;               /**< Guest address of the low end of the stack. */
  be_u32 stack_kernel;              /**< 240 bytes below stack_base, set at creation. */
  be_u32 tls_address;               /**< Guest address of the dynamic TLS block of the thread. */
  u8 thread_state;                  /**< One of the `X_KTHREAD_STATE_FLAGS` values. */
  /** Reserved. @{ */
  u8 alerted[2];
  u8 alertable;
  /** @} */
  u8 priority;          /**< Priority of the thread, clamped to 0 through 31. */
  u8 fpu_exceptions_on; /**< Reserved. */
  u8 process_type_dup;  /**< Same as process_type. */
  u8 process_type;      /**< Process type of the thread, copied from its process. */
  /** APC lists, one per processor mode. */
  kernel::X_TYPED_LIST<XAPC, offsetof(XAPC, list_entry)> apc_lists[2];
  be_ptr<X_KPROCESS> process; /**< Guest address of the process that owns the thread. */
  /** Reserved. @{ */
  u8 executing_kernel_apc;
  u8 deferred_apc_software_interrupt_state;
  u8 user_apc_pending;
  /** @} */
  u8 may_queue_apcs;    /**< Nonzero while APCs can be queued to the thread. */
  X_KSPINLOCK apc_lock; /**< Guards the APC lists and the enqueued state of their APCs. */
  /** Reserved. @{ */
  be_u32 num_context_switches_to;
  X_LIST_ENTRY ready_prcb_entry;
  /** @} */
  be_u32 msr_mask; /**< Set to 0xFDFFD7FF at creation. */
  /** Reserved. @{ */
  rex::BE<X_STATUS> wait_result;
  u8 wait_irql;
  u8 unk_a5[0xB];
  /** @} */
  /** Count of entered critical regions, 0 or below. APCs wait until it is 0. */
  i32 apc_disable_count;
  /** Reserved. @{ */
  be_i32 quantum;
  u8 unk_b8;
  u8 unk_b9;
  u8 unk_ba;
  /** @} */
  u8 boost_disabled; /**< Nonzero while priority boosts are disabled for the thread. */
  u8 suspend_count;  /**< Number of pending suspends, the thread is suspended while nonzero. */
  u8 was_preempted;  /**< Reserved. */
  u8 terminated;     /**< 1 once the thread has exited. */
  u8 current_cpu;
  be_ptr<X_KPRCB> prcb;       /**< Guest address of the processor control block in the `X_KPCR`. */
  be_ptr<X_KPRCB> prcb_alias; /**< Same as prcb. */
  /** Reserved. @{ */
  u8 unk_c8;
  u8 unk_c9;
  u8 unk_ca;
  u8 unk_cb;
  X_KSPINLOCK timer_list_lock;
  /** @} */
  be_u32 stack_alloc_base; /**< Guest address of the base of the stack allocation. */
  /** Reserved. @{ */
  XAPC on_suspend;
  X_KSEMAPHORE suspend_sema;
  /** @} */
  X_LIST_ENTRY process_threads; /**< Links the thread in the thread list of its process. */
  /** Reserved. @{ */
  be_u32 unk_118;
  X_LIST_ENTRY queue_related;
  be_u32 unk_124;
  be_u32 unk_128;
  be_u32 unk_12c;
  /** @} */
  be_u64 create_time; /**< Guest system time at creation. */
  be_u64 exit_time;
  be_u32 exit_status;
  X_LIST_ENTRY timer_list; /**< List head set empty at creation. */
  be_u32 thread_id;
  be_u32 start_address;
  X_LIST_ENTRY unk_154; /**< List head set empty at creation. */
  u8 unk_15c[0x4];      /**< Reserved. */
  be_u32 last_error;    /**< Last error code of the thread, 0 at creation. */
  be_u32 fiber_ptr;     /**< Guest address of the `X_FIBER_CONTEXT` in use, 0 for none. */
  u8 unk_168[0x4];      /**< Reserved. */
  be_u32 creation_flags;
  u8 unk_170[0xC];   /**< Reserved. */
  be_u32 unk_17c;    /**< Set to 1 at creation. */
  u8 unk_180[0x930]; /**< Reserved. */
};
REX_STATIC_ASSERT_SIZE(X_KTHREAD, 0xAB0);

}  // namespace rex::kernel

namespace rex::kernel {

#pragma pack(push, 4)

/** Registration of a routine the kernel calls when the title terminates. */
struct X_EX_TITLE_TERMINATE_REGISTRATION {
  be_u32 notification_routine;
  be_u32 priority;         /**< Signed, higher values run first. */
  X_LIST_ENTRY list_entry; /**< Links the registration into the list of registrations. */
};
REX_STATIC_ASSERT_SIZE(X_EX_TITLE_TERMINATE_REGISTRATION, 16);

struct X_EXCEPTION_RECORD {
  be_u32 code;
  be_u32 exception_flags;
  be_u32 exception_record; /**< Guest address of the nested exception structure. */
  be_u32 exception_address;
  be_u32 number_parameters; /**< Number of used entries in exception_information. */
  be_u32 exception_information[15];
};
REX_STATIC_ASSERT_SIZE(X_EXCEPTION_RECORD, 0x50);

#pragma pack(pop)

}  // namespace rex::kernel

namespace rex::kernel {

/** Process types KeGetCurrentProcessType reports. */
constexpr u32 kXProcTypeIdle = 0;
constexpr u32 kXProcTypeUser = 1;
constexpr u32 kXProcTypeSystem = 2;

struct X_KPROCESS {
  X_KSPINLOCK thread_list_spinlock; /**< Guards thread_list and thread_count. */
  X_LIST_ENTRY thread_list;
  be_i32 quantum;
  be_u32 clrdataa_masked_ptr; /**< Zeroed at setup. */
  be_u32 thread_count;        /**< Number of threads added to thread_list. */
  u8 unk_18;
  u8 unk_19;
  u8 unk_1a;
  u8 unk_1b;                /**< Set to 6 at setup. */
  be_u32 kernel_stack_size; /**< Default stack size of a new thread. */
  be_u32 tls_static_data_address;
  be_u32 tls_data_size;      /**< Size of the TLS data a thread receives. */
  be_u32 tls_raw_data_size;  /**< Reserved. */
  be_u16 tls_slot_size;      /**< Size in bytes of the dynamic TLS slots, 4 per slot. */
  u8 is_terminating;         /**< Reserved. */
  u8 process_type;           /**< One of the kXProcType values. */
  be_u32 tls_slot_bitmap[8]; /**< Set bits mark free TLS slots, slot 0 is the top bit. */
  be_u32 unk_50;             /**< Reserved. */
  X_LIST_ENTRY unk_54;       /**< List head initialized at setup. */
  be_u32 unk_5c;             /**< Reserved. */
};
REX_STATIC_ASSERT_SIZE(X_KPROCESS, 0x60);

/** An object with a self referencing list head that titles read. */
struct X_UNKNOWN_TYPE_REFED {
  be_u32 unk_00;          /**< Set to 0x1000000 at setup, no host code reads it. */
  be_u32 unk_04;          /**< Set to 1 at setup, no host code reads it. */
  be_u32 points_to_self;  /**< Guest address of this field, the empty list head. */
  be_u32 points_to_prior; /**< Same address as points_to_self, the empty list head. */
};
REX_STATIC_ASSERT_SIZE(X_UNKNOWN_TYPE_REFED, 16);

/** The guest kernel globals the runtime allocates at setup. */
struct KernelGuestGlobals {
  /** Pool tag 'Thrd'. */
  X_OBJECT_TYPE ExThreadObjectType;
  /** Pool tag 'Evnt'. */
  X_OBJECT_TYPE ExEventObjectType;
  /** Pool tag 'Muta'. */
  X_OBJECT_TYPE ExMutantObjectType;
  /** Pool tag 'Sema'. */
  X_OBJECT_TYPE ExSemaphoreObjectType;
  /** Pool tag 'Timr'. */
  X_OBJECT_TYPE ExTimerObjectType;
  /** Pool tag 'IoCp'. */
  X_OBJECT_TYPE IoCompletionObjectType;
  /** Pool tag 'IoDv'. */
  X_OBJECT_TYPE IoDeviceObjectType;
  /** Pool tag 'IoFl'. */
  X_OBJECT_TYPE IoFileObjectType;
  /** Pool tag 'ObDr'. */
  X_OBJECT_TYPE ObDirectoryObjectType;
  /** Pool tag 'ObSl'. */
  X_OBJECT_TYPE ObSymbolicLinkObjectType;
  /** Object whose list head points at itself, set up once and read by no host code. */
  X_UNKNOWN_TYPE_REFED self_list_object;
  /** Process block of type kXProcTypeIdle with the quantum set to 0x7F. */
  X_KPROCESS idle_process;
  /** Process block of type kXProcTypeUser, holds the TLS layout of the title. */
  X_KPROCESS title_process;
  X_KPROCESS system_process;   /**< Process block of type kXProcTypeSystem with 32 TLS slots. */
  X_KSPINLOCK dispatcher_lock; /**< Zeroed at setup. No code reads it. */
  X_KSPINLOCK ob_lock;         /**< Zeroed at setup. No code reads it. */
  X_KSPINLOCK tls_lock;        /**< Guards the TLS slot bitmap of each process. */
  u8 UsbdBootEnumerationDoneEvent[0x10]; /**< An X_DISPATCH_HEADER. */
  /** Table XeCryptEccGetCurveParameters points into, entry N holds 0x80000000 + N. */
  be_u32 ecc_curves[15];
};

}  // namespace rex::kernel
