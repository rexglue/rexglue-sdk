/**
 * @file        kernel/object_reference.h
 * @brief       References the Ob and XAM lookups hand to the title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/types.h>

namespace rex::kernel {

/**
 * Takes a reference on the first handle of a pinned object for ObOpenObjectByPointer.
 *
 * @param table The table that owns the object.
 * @param object A pin on the object.
 * @return The first handle, or X_STATUS_INVALID_HANDLE when the object has no handle left.
 */
Result<X_HANDLE> OpenPrimaryHandle(kernel::IObjectTable& table, const kernel::ObjectPin& object);

/**
 * Takes the reference the title releases with ObDereferenceObject on a pinned object.
 *
 * @param table The table that owns the object.
 * @param object A pin on the object.
 * @param object_ptr The guest address the lookup hands out.
 * @return object_ptr, or X_STATUS_INVALID_HANDLE when the object has no handle left.
 */
Result<u32> ReferencePrimary(kernel::IObjectTable& table, const kernel::ObjectPin& object,
                             u32 object_ptr);

/**
 * Takes the reference the title releases with ObDereferenceObject on a thread handle.
 *
 * @param table The table that owns the thread.
 * @param handle The thread handle.
 * @param object_ptr The guest address of the thread object.
 * @return object_ptr, or X_STATUS_INVALID_HANDLE when the handle is closed.
 */
Result<u32> ReferenceThread(kernel::IObjectTable& table, X_HANDLE handle, u32 object_ptr);

}  // namespace rex::kernel
