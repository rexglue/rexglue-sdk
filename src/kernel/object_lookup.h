/**
 * @file        kernel/object_lookup.h
 * @brief       Macros that find or open a kernel object by handle or name
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/assert.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/io.h>

#include "kernel/title_state.h"

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
#define REX_KERNEL_LOOKUP(VAR, TYPE, HANDLE)                                 \
  auto VAR = ::rex::kernel::GetCurrent().GetObjectTable().Get<TYPE>(HANDLE); \
  if (!(VAR)) {                                                              \
    return rex::kernel::X_STATUS_INVALID_HANDLE;                             \
  }
// NOLINTEND(bugprone-macro-parentheses)

/**
 * Returns early from a named create export when an object with that name exists.
 *
 * Without OBJ_OPENIF an existing name returns X_STATUS_OBJECT_NAME_COLLISION for any type.
 * With OBJ_OPENIF an object of the same type stores a new handle to it and returns
 * X_STATUS_OBJECT_NAME_EXISTS, and an object of another type returns
 * X_STATUS_OBJECT_TYPE_MISMATCH. Both failures store handle 0. Falls through only when the name is
 * absent.
 *
 * @param KIND The ObjectType enumerator of the object.
 * @param HANDLE_OUT The handle out parameter.
 * @param ATTRS The object attributes pointer.
 *
 * @code
 * REX_KERNEL_NAMED_CREATE(Event, handle_ptr, obj_attributes_ptr);
 * @endcode
 */
#define REX_KERNEL_NAMED_CREATE(KIND, HANDLE_OUT, ATTRS)                                      \
  if (ATTRS) {                                                                                \
    auto& rex_runtime_ = ::rex::kernel::GetCurrent();                                         \
    auto rex_attrs_ =                                                                         \
        StateOf(rex_runtime_)                                                                 \
            .GetMemory()                                                                      \
            ->TranslateVirtual<rex::kernel::X_OBJECT_ATTRIBUTES*>((ATTRS).GetGuestAddress()); \
    REX_ASSERT(rex_attrs_->name_ptr != 0);                                                    \
    auto rex_name_ = kernel::TranslateAnsiStringAddress(StateOf(rex_runtime_).GetMemory(),    \
                                                        rex_attrs_->name_ptr);                \
    auto& rex_table_ = rex_runtime_.GetObjectTable();                                         \
    if (!rex_name_.empty()) {                                                                 \
      if (auto rex_opened_ = rex_table_.OpenByName(rex_name_)) {                              \
        const rex::kernel::X_HANDLE rex_handle_ = *rex_opened_;                               \
        auto rex_existing_ = rex_table_.Get(rex_handle_);                                     \
        const bool rex_same_type_ =                                                           \
            rex_existing_ && rex_existing_.GetType() == ::rex::kernel::ObjectType::KIND;      \
        const bool rex_open_if_ =                                                             \
            (static_cast<u32>(rex_attrs_->attributes) & ::rex::kernel::kObjOpenIf) != 0;      \
        if (rex_same_type_ && rex_open_if_ && (HANDLE_OUT)) {                                 \
          *(HANDLE_OUT) = rex_handle_;                                                        \
          return rex::kernel::X_STATUS_OBJECT_NAME_EXISTS;                                    \
        }                                                                                     \
        ::rex::kernel::CloseOwned(rex_table_, rex_handle_);                                   \
        if (rex_same_type_ && rex_open_if_) {                                                 \
          return rex::kernel::X_STATUS_OBJECT_NAME_EXISTS;                                    \
        }                                                                                     \
        if (HANDLE_OUT) {                                                                     \
          *(HANDLE_OUT) = 0u;                                                                 \
        }                                                                                     \
        return rex_open_if_ ? rex::kernel::X_STATUS_OBJECT_TYPE_MISMATCH                      \
                            : rex::kernel::X_STATUS_OBJECT_NAME_COLLISION;                    \
      }                                                                                       \
    }                                                                                         \
  }
