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

#include <variant>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/runtime/thread_registry.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/kernel_string.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/object.h>
#include <rex/runtime/semaphore_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/argument_check.h"
#include "kernel/object_reference.h"

namespace rex::kernel {

Result<X_HANDLE> OpenPrimaryHandle(runtime::IObjectTable& table,
                                   const runtime::PinnedSlot& object) {
  if (auto retained = table.RetainPrimary(object); !retained) {
    return Err<X_HANDLE>(retained.error());
  }
  return table.GetPrimaryHandle(object).value_or(0);
}

Result<u32> ReferencePrimary(runtime::IObjectTable& table, const runtime::PinnedSlot& object,
                             u32 object_ptr) {
  if (auto retained = table.RetainPrimary(object); !retained) {
    return Err<u32>(retained.error());
  }
  return object_ptr;
}

Result<u32> ReferenceThread(runtime::IObjectTable& table, X_HANDLE handle, u32 object_ptr) {
  if (auto retained = table.Retain(handle); !retained) {
    return Err<u32>(retained.error());
  }
  return object_ptr;
}

}  // namespace rex::kernel

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

/**
 * Opens an object by name.
 *
 * @param obj_attributes_ptr The X_OBJECT_ATTRIBUTES whose name_ptr names the object.
 * @param object_type_ptr ExEventObjectType, ExSemaphoreObjectType or ExTimerObjectType.
 * @param unk Always 0.
 * @param handle_ptr Receives the handle.
 * @return The translated status of the open.
 */
u32 ObOpenObjectByName_entry(MappedVoid obj_attributes_ptr,
                             [[maybe_unused]] MappedVoid object_type_ptr, [[maybe_unused]] u32 unk,
                             MappedU32 handle_ptr) {
  REX_KERNEL_REQUIRE_NONNULL(obj_attributes_ptr);

  auto obj_attributes = runtime::GetCurrent().GetMemory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr.GetGuestAddress());
  REX_ASSERT(obj_attributes->name_ptr != 0);
  auto name = runtime::TranslateAnsiStringAddress(runtime::GetCurrent().GetMemory(),
                                                  obj_attributes->name_ptr);

  auto opened = runtime::GetCurrent().GetObjectTable().OpenByName(name);
  if (opened) {
    *handle_ptr = *opened;
  }

  return ToXStatus(opened);
}

u32 ObOpenObjectByPointer_entry(MappedVoid object_ptr, MappedU32 out_handle_ptr) {
  auto& table = runtime::GetCurrent().GetObjectTable();
  const X_HANDLE handle = table.ResolveHandle(object_ptr.GetGuestAddress(), -1).value_or(0);
  auto object = table.Get(handle);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto opened = OpenPrimaryHandle(table, object);
  if (!opened) {
    return ToXStatus(opened);
  }
  *out_handle_ptr = *opened;
  return X_STATUS_SUCCESS;
}

u32 ObLookupThreadByThreadId_entry(u32 thread_id, MappedU32 out_object_ptr) {
  auto thread = runtime::GetCurrent().GetThreadRegistry().Find(thread_id);
  if (!thread) {
    return X_STATUS_NOT_FOUND;
  }

  auto referenced = ReferenceThread(runtime::GetCurrent().GetObjectTable(), thread->GetHandle(),
                                    thread->GetGuestObject());
  if (!referenced) {
    return ToXStatus(referenced);
  }
  *out_object_ptr = *referenced;
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByHandle_entry(u32 handle, u32 object_type_ptr, MappedU32 out_object_ptr) {
  REXLOG_TRACE(kernel,
               "ObReferenceObjectByHandle "
               "handle={:#x} type={:#x}",
               static_cast<u32>(handle), static_cast<u32>(object_type_ptr));

  auto object = runtime::GetCurrent().GetObjectTable().Get(handle);
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  u32 native_ptr = object.GetGuestAddress();
  if (object.GetKind() == ObjectKind::kThread) {
    auto thread = runtime::GetCurrent().GetThreadRegistry().Find(
        std::get<ThreadRef>(object.GetData()).thread_id);
    native_ptr = thread ? thread->GetGuestObject() : 0;
  }

  if (object_type_ptr) {
    u32 globals_base = runtime::GetCurrent().GetKernelGlobals();
    u32 expected_type = 0;
    switch (object.GetKind()) {
      case ObjectKind::kThread:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExThreadObjectType);
        break;
      case ObjectKind::kEvent:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExEventObjectType);
        break;
      case ObjectKind::kMutant:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExMutantObjectType);
        break;
      case ObjectKind::kSemaphore:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExSemaphoreObjectType);
        break;
      case ObjectKind::kTimer:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExTimerObjectType);
        break;
      default:
        break;
    }
    if (expected_type && object_type_ptr != expected_type) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  }

  auto referenced = ReferencePrimary(runtime::GetCurrent().GetObjectTable(), object, native_ptr);
  if (!referenced) {
    return ToXStatus(referenced);
  }
  if (out_object_ptr.GetGuestAddress()) {
    *out_object_ptr = *referenced;
  }
  REXLOG_TRACE(kernel,
               "ObReferenceObjectByHandle -> "
               "0x0 obj={:#x}",
               native_ptr);
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByName_entry(MappedString name, [[maybe_unused]] u32 attributes,
                                  u32 object_type_ptr, [[maybe_unused]] MappedVoid parse_context,
                                  MappedU32 out_object_ptr) {
  auto opened = runtime::GetCurrent().GetObjectTable().OpenByName(name.GetValue());
  if (opened) {
    return ObReferenceObjectByHandle_entry(*opened, object_type_ptr, out_object_ptr);
  }

  return ToXStatus(opened);
}

u32 ObDereferenceObject_entry(u32 native_ptr) {
  REXLOG_TRACE(kernel,
               "ObDereferenceObject "
               "ptr={:#x}",
               static_cast<u32>(native_ptr));
  auto& table = runtime::GetCurrent().GetObjectTable();
  auto object = table.Get(table.ResolveHandle(native_ptr, -1).value_or(0));
  if (object) {
    if (auto released = table.ReleasePrimary(object); !released) {
      REXLOG_WARN(kernel, "ObDereferenceObject: {:#x} not released, {}", native_ptr,
                  released.error().message);
    }
  }

  return 0;
}

u32 ObCreateSymbolicLink_entry(GuestPtrT<X_ANSI_STRING> path_ptr,
                               GuestPtrT<X_ANSI_STRING> target_ptr) {
  auto path = rex::utf8_canonicalize_guest_path(
      runtime::TranslateAnsiPath(runtime::GetCurrent().GetMemory(), path_ptr));
  auto target = rex::utf8_canonicalize_guest_path(
      runtime::TranslateAnsiPath(runtime::GetCurrent().GetMemory(), target_ptr));

  if (rex::utf8_starts_with(path, u8"\\??\\")) {
    path = path.substr(4);
  }

  if (auto linked = runtime::GetCurrent().GetFileSystem()->Link(path, target);
      !linked && filesystem::CodeOf(linked.error()) != filesystem::FileSystemError::kExists) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

u32 ObDeleteSymbolicLink_entry(GuestPtrT<X_ANSI_STRING> path_ptr) {
  auto path = rex::utf8_canonicalize_guest_path(
      runtime::TranslateAnsiPath(runtime::GetCurrent().GetMemory(), path_ptr));
  if (rex::utf8_starts_with(path, u8"\\??\\")) {
    path = path.substr(4);
  }
  if (!runtime::GetCurrent().GetFileSystem()->Unlink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

/**
 * Creates a new handle to the same object.
 *
 * @param handle The source handle, -2 is the current thread pseudo handle.
 * @param new_handle_ptr Receives the new handle when not null.
 * @param options 1 closes the source handle (DUPLICATE_CLOSE_SOURCE).
 * @return X_STATUS_SUCCESS or an error status.
 */
u32 NtDuplicateObject_entry(u32 handle, MappedU32 new_handle_ptr, u32 options) {
  auto& table = runtime::GetCurrent().GetObjectTable();
  auto duplicated = table.Duplicate(handle);

  if (new_handle_ptr) {
    *new_handle_ptr = duplicated.value_or(kXInvalidHandleValue);
  }

  if (options == 1) {
    if (auto removed = table.Remove(handle); !removed) {
      REXLOG_WARN(kernel, "NtDuplicateObject: source handle {:08X} not closed, {}", handle,
                  removed.error().message);
    }
  }

  return ToXStatus(duplicated);
}

u32 NtClose_entry(u32 handle) {
  REXLOG_TRACE(kernel,
               "NtClose "
               "handle={:#x}",
               static_cast<u32>(handle));
  auto result = ToXStatus(runtime::GetCurrent().GetObjectTable().Close(handle));
  REXLOG_TRACE(kernel,
               "NtClose -> "
               "{:#x}",
               result);
  return result;
}

u32 NtQueryEvent_entry(u32 handle, MappedU32 out_event_info_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::GetCurrent().GetObjectTable().Get<EventObject>(handle);
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
REX_EXPORT_STUB(__imp__ObReferenceObject);
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
