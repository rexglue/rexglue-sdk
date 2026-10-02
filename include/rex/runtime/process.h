/**
 * @file        runtime/process.h
 * @brief       The title process blocks and the kernel globals
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** Process types KeGetCurrentProcessType reports. */
constexpr u32 X_PROCTYPE_IDLE = 0;
constexpr u32 X_PROCTYPE_USER = 1;
constexpr u32 X_PROCTYPE_SYSTEM = 2;

/** The guest KPROCESS block. */
struct X_KPROCESS {
  X_KSPINLOCK thread_list_spinlock;     /**< 0x00 */
  X_LIST_ENTRY thread_list;             /**< 0x04 */
  rex::be<i32> quantum;                 /**< 0x0C */
  rex::be<u32> clrdataa_masked_ptr;     /**< 0x10 */
  rex::be<u32> thread_count;            /**< 0x14 */
  u8 unk_18;                            /**< 0x18 */
  u8 unk_19;                            /**< 0x19 */
  u8 unk_1A;                            /**< 0x1A */
  u8 unk_1B;                            /**< 0x1B */
  rex::be<u32> kernel_stack_size;       /**< 0x1C */
  rex::be<u32> tls_static_data_address; /**< 0x20 */
  rex::be<u32> tls_data_size;           /**< 0x24 */
  rex::be<u32> tls_raw_data_size;       /**< 0x28 */
  rex::be<u16> tls_slot_size;           /**< 0x2C */
  u8 is_terminating;                    /**< 0x2E */
  u8 process_type;                      /**< 0x2F */
  rex::be<u32> tls_slot_bitmap[8];      /**< 0x30 */
  rex::be<u32> unk_50;                  /**< 0x50 */
  X_LIST_ENTRY unk_54;                  /**< 0x54 */
  rex::be<u32> unk_5C;                  /**< 0x5C */
};
static_assert_size(X_KPROCESS, 0x60);

/** An object with a self referencing list head that titles probe. */
struct X_UNKNOWN_TYPE_REFED {
  rex::be<u32> field0;
  rex::be<u32> field4;
  rex::be<u32> points_to_self;
  rex::be<u32> points_to_prior;
};
static_assert_size(X_UNKNOWN_TYPE_REFED, 16);

/** The guest kernel globals the runtime allocates at setup. */
struct KernelGuestGlobals {
  X_OBJECT_TYPE ExThreadObjectType;
  X_OBJECT_TYPE ExEventObjectType;
  X_OBJECT_TYPE ExMutantObjectType;
  X_OBJECT_TYPE ExSemaphoreObjectType;
  X_OBJECT_TYPE ExTimerObjectType;
  X_OBJECT_TYPE IoCompletionObjectType;
  X_OBJECT_TYPE IoDeviceObjectType;
  X_OBJECT_TYPE IoFileObjectType;
  X_OBJECT_TYPE ObDirectoryObjectType;
  X_OBJECT_TYPE ObSymbolicLinkObjectType;
  X_UNKNOWN_TYPE_REFED OddObj;
  X_KPROCESS idle_process;
  X_KPROCESS title_process;
  X_KPROCESS system_process;
  X_KSPINLOCK dispatcher_lock;
  X_KSPINLOCK ob_lock;
  X_KSPINLOCK tls_lock;                  /**< Guards the TLS slot bitmap of each process. */
  u8 UsbdBootEnumerationDoneEvent[0x10]; /**< An X_DISPATCH_HEADER. */
};

}  // namespace rex::runtime
