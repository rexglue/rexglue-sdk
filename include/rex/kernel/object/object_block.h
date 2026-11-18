/**
 * @file        kernel/object/object_block.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::kernel {

class GuestAddressSpace;
class WorkspaceHeap;

struct ObjectBlock {
  u32 address = 0;                    /**< Address of the object body, after the X_OBJECT_HEADER. */
  bool owned = false;                 /**< True when the table frees the block with the object. */
  WorkspaceHeap* workspace = nullptr; /**< Null for the system heap. */
};

/** What AllocObjectBlock needs beyond the size and the type. */
struct ObjectBlockOptions {
  /**
   * 0x1000 takes the block from the title pool, 0x2000 from the system pool, and 0 from the pool of
   * the calling thread's process, like the object attributes of ObCreateObject.
   */
  u32 attributes = 0;
  /** Bytes of the last path component of the object name, 0 for an unnamed object. */
  u32 name_length = 0;
  /** True for a thread object, which the console moves up by 8 bytes when it is not page aligned.
   */
  bool thread = false;
};

enum class ObjectBlockHeap : u8 {
  kSystem,
  kWorkspace,
};

/**
 * Allocates a guest block for an object, with its X_OBJECT_HEADER pointing at the type.
 *
 * @param type_address The guest address of the exported type object of the object, such as
 *        ExEventObjectType, 0 for an object that has none.
 * @param workspace The workspace for the header and body, null for the kernel pool or the system
 *        heap.
 * @param options The pool choice, the name length and the thread shift.
 * @return The block with owned set, address 0 when the allocation failed. A block of an object
 *         type with a type object comes from the kernel pool with the tag of the type, and the
 *         title pool counts its pages against the title budget. A block without a type object
 *         comes from the system heap and no budget counts it.
 */
REX_KERNEL_API ObjectBlock AllocObjectBlock(kernel::GuestAddressSpace* memory, u32 size,
                                            u32 type_address, WorkspaceHeap* workspace = nullptr,
                                            const ObjectBlockOptions& options = {});

/**
 * Frees a block made by AllocObjectBlock. The type object it points at is not freed.
 *
 * @param address The address of the object body, 0 for none.
 * @param workspace The workspace AllocObjectBlock used, null for the system heap.
 */
REX_KERNEL_API void FreeObjectBlock(kernel::GuestAddressSpace* memory, u32 address,
                                    WorkspaceHeap* workspace = nullptr);

}  // namespace rex::kernel
