/**
 * @file        kernel/kernel_macros.h
 * @brief       Private helper macros shared by the kernel export bodies
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>

/**
 * Looks a handle up in the object table and returns X_STATUS_INVALID_HANDLE when it is absent.
 *
 * @param VAR The name of the reference to declare.
 * @param TYPE The kernel object class.
 * @param HANDLE The handle expression.
 *
 * @code
 * REX_KERNEL_LOOKUP(file, XFile, file_handle);
 * @endcode
 */
#define REX_KERNEL_LOOKUP(VAR, TYPE, HANDLE)                   \
  auto VAR = REX_KERNEL_OBJECTS()->LookupObject<TYPE>(HANDLE); \
  if (!(VAR)) {                                                \
    return X_STATUS_INVALID_HANDLE;                            \
  }

/**
 * Returns X_STATUS_INVALID_PARAMETER when a pointer argument is null.
 *
 * @param PTR The pointer argument.
 *
 * @code
 * REX_KERNEL_ARG(stats_ptr);
 * @endcode
 */
#define REX_KERNEL_ARG(PTR)            \
  if (!(PTR)) {                        \
    return X_STATUS_INVALID_PARAMETER; \
  }

/**
 * Logs that an entry point is partly or fully unimplemented.
 *
 * @param FMT A string literal format.
 * @param ... The format arguments.
 *
 * @code
 * REX_KERNEL_STUB("XamSwapDisc({})", disc_number);
 * @endcode
 */
#define REX_KERNEL_STUB(FMT, ...) \
  REXLOG_WARN(kernel, "unimplemented: " FMT __VA_OPT__(, ) __VA_ARGS__)

/**
 * Handles an existing named object at the head of a named create entry point.
 *
 * Returns X_STATUS_OBJECT_NAME_EXISTS with a retained handle when an object of the same kind
 * carries the name, and X_STATUS_INVALID_HANDLE when an object of another kind does. Falls
 * through when the name is absent.
 *
 * @param CLASS The kernel object class.
 * @param KIND The XObject::Type enumerator of the class.
 * @param HANDLE_OUT The handle out parameter.
 * @param ATTRS The object attributes pointer.
 *
 * @code
 * REX_KERNEL_NAMED_CREATE(XEvent, Event, handle_ptr, obj_attributes_ptr);
 * @endcode
 */
#define REX_KERNEL_NAMED_CREATE(CLASS, KIND, HANDLE_OUT, ATTRS)                                   \
  if (ATTRS) {                                                                                    \
    auto rex_attrs_ =                                                                             \
        REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>((ATTRS).guest_address());     \
    assert_true(rex_attrs_->name_ptr != 0);                                                       \
    auto rex_name_ = util::TranslateAnsiStringAddress(REX_KERNEL_MEMORY(), rex_attrs_->name_ptr); \
    X_HANDLE rex_handle_ = X_INVALID_HANDLE_VALUE;                                                \
    if (!rex_name_.empty() &&                                                                     \
        XSUCCEEDED(REX_KERNEL_OBJECTS()->GetObjectByName(rex_name_, &rex_handle_))) {             \
      if (auto existing_object = REX_KERNEL_OBJECTS()->LookupObject<CLASS>(rex_handle_)) {        \
        existing_object->ReleaseHandle();                                                         \
        if (existing_object->type() == XObject::Type::KIND) {                                     \
          if (HANDLE_OUT) {                                                                       \
            existing_object->RetainHandle();                                                      \
            *(HANDLE_OUT) = existing_object->handle();                                            \
          }                                                                                       \
          return X_STATUS_OBJECT_NAME_EXISTS;                                                     \
        }                                                                                         \
        return X_STATUS_INVALID_HANDLE;                                                           \
      }                                                                                           \
    }                                                                                             \
  }
