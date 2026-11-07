/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <utility>
#include <variant>

#include <rex/assert.h>
#include <rex/filesystem/file_system_error.h>
#include <rex/string/path.h>
#include <rex/logging/log_category.h>
#include <rex/string/text.h>
#include "kernel/categories.h"
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include "kernel/argument_check.h"
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

namespace {
log::OncePerKey<16> bodyless_once;
log::OncePerKey<1> unregistered_once;
}  // namespace

/**
 * Opens a named object and returns a handle to it.
 *
 * @param obj_attributes_ptr The object attributes holding the name.
 * @param object_type_ptr ExEventObjectType, ExSemaphoreObjectType or ExTimerObjectType.
 * @param unk Unused.
 * @param handle_ptr Receives the handle.
 * @return The status.
 */
u32 ObOpenObjectByName_entry(MappedVoid obj_attributes_ptr,
                             [[maybe_unused]] MappedVoid object_type_ptr, [[maybe_unused]] u32 unk,
                             MappedU32 handle_ptr) {
  REX_KERNEL_REQUIRE_NONNULL(obj_attributes_ptr);

  auto obj_attributes =
      kernel::GetCurrentState().GetAddressSpace()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
          obj_attributes_ptr.GetGuestAddress());
  REX_ASSERT(obj_attributes->name_ptr != 0);
  auto name = kernel::TranslateAnsiStringAddress(kernel::GetCurrentState().GetAddressSpace(),
                                                 obj_attributes->name_ptr);

  auto opened = kernel::GetCurrent().GetObjectTable().OpenByName(name);
  if (opened) {
    *handle_ptr = *opened;
  }

  return ToXStatus(opened);
}

u32 ObOpenObjectByPointer_entry(MappedVoid object_ptr, MappedU32 out_handle_ptr) {
  auto& table = kernel::GetCurrent().GetObjectTable();
  auto object = table.ResolveObject(object_ptr.GetGuestAddress(), -1);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto opened = table.Open(object);
  if (!opened) {
    return ToXStatus(opened);
  }
  *out_handle_ptr = *opened;
  return X_STATUS_SUCCESS;
}

u32 ObLookupThreadByThreadId_entry(u32 thread_id, MappedU32 out_object_ptr) {
  auto thread = kernel::GetCurrent().GetThreadRegistry().Find(thread_id);
  if (!thread) {
    return X_STATUS_NOT_FOUND;
  }

  auto& table = kernel::GetCurrent().GetObjectTable();
  auto object = table.GetByAddress(thread->GetGuestObject());
  if (!object) {
    return X_STATUS_NOT_FOUND;
  }
  if (auto referenced = table.Reference(object); !referenced) {
    return ToXStatus(referenced);
  }
  *out_object_ptr = thread->GetGuestObject();
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByHandle_entry(u32 handle, u32 object_type_ptr, MappedU32 out_object_ptr) {
  REXKRNL_TRACE(
      "ObReferenceObjectByHandle "
      "handle={:#x} type={:#x}",
      static_cast<u32>(handle), static_cast<u32>(object_type_ptr));

  auto object = kernel::GetCurrent().GetObjectTable().Get(handle);
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  u32 native_ptr = object.GetGuestAddress();
  if (object.GetType() == ObjectType::kThread) {
    auto thread = kernel::GetCurrent().GetThreadRegistry().Find(
        std::get<ThreadRef>(object.GetData()).thread_id);
    native_ptr = thread ? thread->GetGuestObject() : 0;
  }

  if (object_type_ptr) {
    const u32 expected_type = kernel::GetCurrentState().GetObjectTypeAddress(object.GetType());
    if (expected_type && object_type_ptr != expected_type) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  }

  if (native_ptr) {
    if (auto referenced = kernel::GetCurrent().GetObjectTable().Reference(object); !referenced) {
      return ToXStatus(referenced);
    }
  } else {
    REXKRNL_WARN_ONCE(bodyless_once, std::to_underlying(object.GetType()),
                      "ObReferenceObjectByHandle on object type {} with no guest body returns 0",
                      std::to_underlying(object.GetType()));
  }
  if (out_object_ptr.GetGuestAddress()) {
    *out_object_ptr = native_ptr;
  }
  REXKRNL_TRACE(
      "ObReferenceObjectByHandle -> "
      "0x0 obj={:#x}",
      native_ptr);
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByName_entry(MappedString name, [[maybe_unused]] u32 attributes,
                                  u32 object_type_ptr, [[maybe_unused]] MappedVoid parse_context,
                                  MappedU32 out_object_ptr) {
  auto& table = kernel::GetCurrent().GetObjectTable();
  auto opened = table.OpenByName(name.GetValue());
  if (!opened) {
    return ToXStatus(opened);
  }
  const u32 status = ObReferenceObjectByHandle_entry(*opened, object_type_ptr, out_object_ptr);
  kernel::CloseOwned(table, *opened);
  return status;
}

/**
 * Adds a pointer reference to the object at a guest address.
 *
 * @param object_ptr The guest address of the object body. An address with no registered object
 *        changes nothing.
 */
void ObReferenceObject_entry(u32 object_ptr) {
  auto& table = kernel::GetCurrent().GetObjectTable();
  auto object = table.GetByAddress(object_ptr);
  if (!object) {
    REXKRNL_WARN_ONCE(unregistered_once, 0, "ObReferenceObject: {:#x} is not a registered object",
                      object_ptr);
    return;
  }
  if (auto referenced = table.Reference(object); !referenced) {
    REXKRNL_WARN("ObReferenceObject: {:#x} not referenced, {}", object_ptr,
                 referenced.error().message);
  }
}

u32 ObDereferenceObject_entry(u32 native_ptr) {
  REXKRNL_TRACE(
      "ObDereferenceObject "
      "ptr={:#x}",
      static_cast<u32>(native_ptr));
  auto& table = kernel::GetCurrent().GetObjectTable();
  auto object = table.GetByAddress(native_ptr);
  if (!object) {
    REXKRNL_WARN("ObDereferenceObject: {:#x} is not a registered object", native_ptr);
    return 0;
  }
  if (auto released = table.Dereference(object); !released) {
    REXKRNL_WARN("ObDereferenceObject: {:#x} not released, {}", native_ptr,
                 released.error().message);
  }

  return 0;
}

u32 ObCreateSymbolicLink_entry(GuestPtrT<X_ANSI_STRING> path_ptr,
                               GuestPtrT<X_ANSI_STRING> target_ptr) {
  auto path = rex::string::utf8_canonicalize_guest_path(
      kernel::TranslateAnsiPath(kernel::GetCurrentState().GetAddressSpace(), path_ptr));
  auto target = rex::string::utf8_canonicalize_guest_path(
      kernel::TranslateAnsiPath(kernel::GetCurrentState().GetAddressSpace(), target_ptr));

  if (rex::string::utf8_starts_with(path, u8"\\??\\")) {
    path = path.substr(4);
  }

  if (auto linked = kernel::GetCurrentState().GetFileSystem()->Link(path, target);
      !linked && filesystem::CodeOf(linked.error()) != filesystem::FileSystemError::kExists) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

u32 ObDeleteSymbolicLink_entry(GuestPtrT<X_ANSI_STRING> path_ptr) {
  auto path = rex::string::utf8_canonicalize_guest_path(
      kernel::TranslateAnsiPath(kernel::GetCurrentState().GetAddressSpace(), path_ptr));
  if (rex::string::utf8_starts_with(path, u8"\\??\\")) {
    path = path.substr(4);
  }
  if (!kernel::GetCurrentState().GetFileSystem()->Unlink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

/**
 * Duplicates a handle. The handle -2 is the current thread pseudo handle.
 *
 * @param handle The source handle.
 * @param new_handle_ptr Receives the new handle.
 * @param options 1 closes the source handle.
 * @return The status.
 */
u32 NtDuplicateObject_entry(u32 handle, MappedU32 new_handle_ptr, u32 options) {
  auto& table = kernel::GetCurrent().GetObjectTable();
  auto duplicated = table.Duplicate(handle);

  if (new_handle_ptr) {
    *new_handle_ptr = duplicated.value_or(kXInvalidHandleValue);
  }

  if (options == 1) {
    if (auto removed = table.Close(handle); !removed) {
      REXKRNL_WARN("NtDuplicateObject: source handle {:08X} not closed, {}", handle,
                   removed.error().message);
    }
  }

  return ToXStatus(duplicated);
}

u32 NtClose_entry(u32 handle) {
  REXKRNL_TRACE(
      "NtClose "
      "handle={:#x}",
      static_cast<u32>(handle));
  auto result = ToXStatus(kernel::GetCurrent().GetObjectTable().Close(handle));
  REXKRNL_TRACE(
      "NtClose -> "
      "{:#x}",
      result);
  return result;
}

u32 NtQueryEvent_entry(u32 handle, MappedU32 out_event_info_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel::GetCurrent().GetObjectTable().Get<EventObject>(handle);
  if (ev) {
    u32 type_tmp, state_tmp;
    ev->Query(&type_tmp, &state_tmp);
    out_event_info_ptr[0] = type_tmp;
    out_event_info_ptr[1] = state_tmp;
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__ObOpenObjectByName, rex::kernel::xboxkrnl::ObOpenObjectByName_entry)
REX_EXPORT(__imp__ObOpenObjectByPointer, rex::kernel::xboxkrnl::ObOpenObjectByPointer_entry)
REX_EXPORT(__imp__ObLookupThreadByThreadId, rex::kernel::xboxkrnl::ObLookupThreadByThreadId_entry)
REX_EXPORT(__imp__ObReferenceObjectByHandle, rex::kernel::xboxkrnl::ObReferenceObjectByHandle_entry)
REX_EXPORT(__imp__ObReferenceObjectByName, rex::kernel::xboxkrnl::ObReferenceObjectByName_entry)
REX_EXPORT(__imp__ObReferenceObject, rex::kernel::xboxkrnl::ObReferenceObject_entry)
REX_EXPORT(__imp__ObDereferenceObject, rex::kernel::xboxkrnl::ObDereferenceObject_entry)
REX_EXPORT(__imp__ObCreateSymbolicLink, rex::kernel::xboxkrnl::ObCreateSymbolicLink_entry)
REX_EXPORT(__imp__ObDeleteSymbolicLink, rex::kernel::xboxkrnl::ObDeleteSymbolicLink_entry)
REX_EXPORT(__imp__NtDuplicateObject, rex::kernel::xboxkrnl::NtDuplicateObject_entry)
REX_EXPORT(__imp__NtClose, rex::kernel::xboxkrnl::NtClose_entry)
REX_EXPORT(__imp__NtQueryEvent, rex::kernel::xboxkrnl::NtQueryEvent_entry)

REX_EXPORT_STUB(__imp__ObCreateObject);
REX_EXPORT_STUB(__imp__ObGetWaitableObject);
REX_EXPORT_STUB(__imp__ObInsertObject);
REX_EXPORT_STUB(__imp__ObIsTitleObject);
REX_EXPORT_STUB(__imp__ObLookupAnyThreadByThreadId);
REX_EXPORT_STUB(__imp__ObMakeTemporaryObject);
REX_EXPORT_STUB(__imp__ObTranslateSymbolicLink);
REX_EXPORT_STUB(__imp__NtCreateDirectoryObject);
REX_EXPORT_STUB(__imp__NtCreateSymbolicLinkObject);
REX_EXPORT_STUB(__imp__NtMakeTemporaryObject);
REX_EXPORT_STUB(__imp__NtOpenDirectoryObject);
REX_EXPORT_STUB(__imp__NtQueryDirectoryObject);
REX_EXPORT_STUB(__imp__NtQueryIoCompletion);
REX_EXPORT_STUB(__imp__NtQueryMutant);
REX_EXPORT_STUB(__imp__NtQuerySemaphore);
REX_EXPORT_STUB(__imp__NtQueryTimer);
