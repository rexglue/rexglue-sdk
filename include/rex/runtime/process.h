/**
 * @file        runtime/process.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** Process types KeGetCurrentProcessType reports. */
constexpr u32 kXProcTypeIdle = 0;
constexpr u32 kXProcTypeUser = 1;
constexpr u32 kXProcTypeSystem = 2;

/** The guest KPROCESS block. */
struct X_KPROCESS {
  X_KSPINLOCK thread_list_spinlock;
  X_LIST_ENTRY thread_list;
  be_i32 quantum;
  be_u32 clrdataa_masked_ptr;
  be_u32 thread_count;
  u8 unk_18;
  u8 unk_19;
  u8 unk_1a;
  u8 unk_1b;
  be_u32 kernel_stack_size;
  be_u32 tls_static_data_address;
  be_u32 tls_data_size;
  be_u32 tls_raw_data_size;
  be_u16 tls_slot_size;
  u8 is_terminating;
  u8 process_type;
  be_u32 tls_slot_bitmap[8];
  be_u32 unk_50;
  X_LIST_ENTRY unk_54;
  be_u32 unk_5c;
};
REX_STATIC_ASSERT_SIZE(X_KPROCESS, 0x60);

/** An object with a self referencing list head that titles read. */
struct X_UNKNOWN_TYPE_REFED {
  be_u32 field0;          /**< Set to 0x1000000 at setup, no host code reads it. */
  be_u32 field4;          /**< Set to 1 at setup, no host code reads it. */
  be_u32 points_to_self;  /**< Guest address of this field, the empty list head. */
  be_u32 points_to_prior; /**< Same address as points_to_self, the empty list head. */
};
REX_STATIC_ASSERT_SIZE(X_UNKNOWN_TYPE_REFED, 16);

/** The guest kernel globals the runtime allocates at setup. */
struct KernelGuestGlobals {
  /** Object type descriptor exported to titles as ExThreadObjectType, pool tag 'Thrd'. */
  X_OBJECT_TYPE ExThreadObjectType;
  /** Object type descriptor exported to titles as ExEventObjectType, pool tag 'Evnt'. */
  X_OBJECT_TYPE ExEventObjectType;
  /** Object type descriptor exported to titles as ExMutantObjectType, pool tag 'Muta'. */
  X_OBJECT_TYPE ExMutantObjectType;
  /** Object type descriptor exported to titles as ExSemaphoreObjectType, pool tag 'Sema'. */
  X_OBJECT_TYPE ExSemaphoreObjectType;
  /** Object type descriptor exported to titles as ExTimerObjectType, pool tag 'Timr'. */
  X_OBJECT_TYPE ExTimerObjectType;
  /** Object type descriptor exported to titles as IoCompletionObjectType, pool tag 'IoCp'. */
  X_OBJECT_TYPE IoCompletionObjectType;
  /** Object type descriptor exported to titles as IoDeviceObjectType, pool tag 'IoDv'. */
  X_OBJECT_TYPE IoDeviceObjectType;
  /** Object type descriptor exported to titles as IoFileObjectType, pool tag 'IoFl'. */
  X_OBJECT_TYPE IoFileObjectType;
  /** Object type descriptor exported to titles as ObDirectoryObjectType, pool tag 'ObDr'. */
  X_OBJECT_TYPE ObDirectoryObjectType;
  /** Object type descriptor exported to titles as ObSymbolicLinkObjectType, pool tag 'ObSl'. */
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
  /** Elliptic curve table XeCryptEccGetCurveParameters points into, entry N holds 0x80000000 + N.
   */
  be_u32 ecc_curves[15];
};

}  // namespace rex::runtime
