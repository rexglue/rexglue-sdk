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
#include <rex/runtime.h>
#include <rex/runtime/kernel_string.h>

/**
 * Looks a handle up in the object table and returns X_STATUS_INVALID_HANDLE when it is absent.
 *
 * @param VAR The name of the pin to declare.
 * @param TYPE The object struct the handle must hold.
 * @param HANDLE The handle expression.
 *
 * @code
 * REX_KERNEL_LOOKUP(file, FileObject, file_handle);
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): VAR is a declared name
#define REX_KERNEL_LOOKUP(VAR, TYPE, HANDLE)                                  \
  auto VAR = ::rex::runtime::GetCurrent().GetObjectTable().Get<TYPE>(HANDLE); \
  if (!(VAR)) {                                                               \
    return X_STATUS_INVALID_HANDLE;                                           \
  }
// NOLINTEND(bugprone-macro-parentheses)

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
 * carries the name. Falls through when the name is absent or an object of another kind holds it.
 *
 * @param KIND The ObjectKind enumerator of the object.
 * @param HANDLE_OUT The handle out parameter.
 * @param ATTRS The object attributes pointer.
 *
 * @code
 * REX_KERNEL_NAMED_CREATE(Event, handle_ptr, obj_attributes_ptr);
 * @endcode
 */
#define REX_KERNEL_NAMED_CREATE(KIND, HANDLE_OUT, ATTRS)                                     \
  if (ATTRS) {                                                                               \
    auto& rex_runtime_ = ::rex::runtime::GetCurrent();                                       \
    auto rex_attrs_ = rex_runtime_.GetMemory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(      \
        (ATTRS).GetGuestAddress());                                                          \
    REX_ASSERT(rex_attrs_->name_ptr != 0);                                                   \
    auto rex_name_ =                                                                         \
        runtime::TranslateAnsiStringAddress(rex_runtime_.GetMemory(), rex_attrs_->name_ptr); \
    auto& rex_table_ = rex_runtime_.GetObjectTable();                                        \
    if (!rex_name_.empty()) {                                                                \
      if (auto rex_opened_ = rex_table_.OpenByName(rex_name_)) {                             \
        const X_HANDLE rex_handle_ = *rex_opened_;                                           \
        auto rex_existing_ = rex_table_.Get(rex_handle_);                                    \
        const bool rex_same_kind_ =                                                          \
            rex_existing_ && rex_existing_.GetKind() == ::rex::runtime::ObjectKind::KIND;    \
        if (rex_same_kind_ && (HANDLE_OUT)) {                                                \
          *(HANDLE_OUT) = rex_handle_;                                                       \
          return X_STATUS_OBJECT_NAME_EXISTS;                                                \
        }                                                                                    \
        ::rex::runtime::CloseOwned(rex_table_, rex_handle_);                                 \
        if (rex_same_kind_) {                                                                \
          return X_STATUS_OBJECT_NAME_EXISTS;                                                \
        }                                                                                    \
      }                                                                                      \
    }                                                                                        \
  }
