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

class Memory;
class WorkspaceHeap;

struct ObjectBlock {
  u32 address = 0;                    /**< Address of the object body, after the X_OBJECT_HEADER. */
  bool owned = false;                 /**< True when the table frees the block with the object. */
  WorkspaceHeap* workspace = nullptr; /**< Null for the system heap. */
};

enum class ObjectBlockHeap : u8 {
  kSystem,
  kWorkspace,
};

/**
 * Allocates a guest block for an object, with its X_OBJECT_HEADER and X_OBJECT_TYPE.
 *
 * @param workspace The workspace for the header and body, null for the system heap.
 * @return The block with owned set, address 0 when the allocation failed.
 */
REX_KERNEL_API ObjectBlock AllocObjectBlock(kernel::Memory* memory, u32 size,
                                            WorkspaceHeap* workspace = nullptr);

/**
 * Frees a block made by AllocObjectBlock, with its X_OBJECT_TYPE.
 *
 * @param address The address of the object body, 0 for none.
 * @param workspace The workspace AllocObjectBlock used, null for the system heap.
 */
REX_KERNEL_API void FreeObjectBlock(kernel::Memory* memory, u32 address,
                                    WorkspaceHeap* workspace = nullptr);

}  // namespace rex::kernel
