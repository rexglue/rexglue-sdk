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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <variant>

#include <rex/assert.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/util/string_utils.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xsemaphore.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

u32 ObOpenObjectByName_entry(mapped_void obj_attributes_ptr, mapped_void object_type_ptr, u32 unk,
                             mapped_u32 handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  REX_KERNEL_ARG(obj_attributes_ptr);

  auto obj_attributes = runtime::Current().memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr.guest_address());
  assert_true(obj_attributes->name_ptr != 0);
  auto name =
      util::TranslateAnsiStringAddress(runtime::Current().memory(), obj_attributes->name_ptr);

  auto opened = runtime::Current().object_table().OpenByName(name);
  if (opened) {
    *handle_ptr = *opened;
  }

  return ToXStatus(opened);
}

u32 ObOpenObjectByPointer_entry(mapped_void object_ptr, mapped_u32 out_handle_ptr) {
  auto& table = runtime::Current().object_table();
  const X_HANDLE handle = table.ResolveGuestObject(object_ptr.guest_address(), -1).value_or(0);
  auto object = table.Get(handle);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  (void)table.RetainPrimary(object);
  *out_handle_ptr = table.PrimaryHandle(object).value_or(0);
  return X_STATUS_SUCCESS;
}

u32 ObLookupThreadByThreadId_entry(u32 thread_id, mapped_u32 out_object_ptr) {
  auto thread = runtime::Current().thread_registry().Find(thread_id);
  if (!thread) {
    return X_STATUS_NOT_FOUND;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  (void)runtime::Current().object_table().Retain(thread->handle());
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByHandle_entry(u32 handle, u32 object_type_ptr, mapped_u32 out_object_ptr) {
  REXLOG_TRACE(kernel,
               "ObReferenceObjectByHandle "
               "handle={:#x} type={:#x}",
               (uint32_t)handle, (uint32_t)object_type_ptr);

  auto object = runtime::Current().object_table().Get(handle);
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t native_ptr = object.guest_object();
  if (object.kind() == ObjectKind::Thread) {
    auto thread =
        runtime::Current().thread_registry().Find(std::get<ThreadRef>(object.data()).thread_id);
    native_ptr = thread ? thread->guest_object() : 0;
  }

  // Type check using real KernelGuestGlobals addresses.
  if (object_type_ptr) {
    uint32_t globals_base = runtime::Current().GetKernelGuestGlobals();
    uint32_t expected_type = 0;
    switch (object.kind()) {
      case ObjectKind::Thread:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExThreadObjectType);
        break;
      case ObjectKind::Event:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExEventObjectType);
        break;
      case ObjectKind::Mutant:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExMutantObjectType);
        break;
      case ObjectKind::Semaphore:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExSemaphoreObjectType);
        break;
      case ObjectKind::Timer:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExTimerObjectType);
        break;
      default:
        break;
    }
    if (expected_type && object_type_ptr != expected_type) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  (void)runtime::Current().object_table().RetainPrimary(object);
  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }
  REXLOG_TRACE(kernel,
               "ObReferenceObjectByHandle -> "
               "0x0 obj={:#x}",
               native_ptr);
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByName_entry(mapped_string name, u32 attributes, u32 object_type_ptr,
                                  mapped_void parse_context, mapped_u32 out_object_ptr) {
  auto opened = runtime::Current().object_table().OpenByName(name.value());
  if (opened) {
    return ObReferenceObjectByHandle_entry(*opened, object_type_ptr, out_object_ptr);
  }

  return ToXStatus(opened);
}

u32 ObDereferenceObject_entry(u32 native_ptr) {
  REXLOG_TRACE(kernel,
               "ObDereferenceObject "
               "ptr={:#x}",
               (uint32_t)native_ptr);
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return 0;
  }

  auto& table = runtime::Current().object_table();
  auto object = table.Get(table.ResolveGuestObject(native_ptr, -1).value_or(0));
  if (object) {
    (void)table.ReleasePrimary(object);
  }

  return 0;
}

u32 ObCreateSymbolicLink_entry(ppc_ptr_t<X_ANSI_STRING> path_ptr,
                               ppc_ptr_t<X_ANSI_STRING> target_ptr) {
  auto path = rex::utf8_canonicalize_guest_path(
      util::TranslateAnsiPath(runtime::Current().memory(), path_ptr));
  auto target = rex::utf8_canonicalize_guest_path(
      util::TranslateAnsiPath(runtime::Current().memory(), target_ptr));

  if (rex::utf8_starts_with(path, u8"\\??\\")) {
    path = path.substr(4);  // Strip the full qualifier
  }

  if (auto linked = runtime::Current().file_system()->Link(path, target);
      !linked && filesystem::CodeOf(linked.error()) != filesystem::Error::kExists) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

u32 ObDeleteSymbolicLink_entry(ppc_ptr_t<X_ANSI_STRING> path_ptr) {
  auto path = util::TranslateAnsiPath(runtime::Current().memory(), path_ptr);
  if (!runtime::Current().file_system()->Unlink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

u32 NtDuplicateObject_entry(u32 handle, mapped_u32 new_handle_ptr, u32 options) {
  // NOTE: new_handle_ptr can be zero to just close a handle.
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // This function actually just creates a new handle to the same object.
  // Most games use it to get real handles to the current thread or whatever.

  auto& table = runtime::Current().object_table();
  auto duplicated = table.Duplicate(handle);

  if (new_handle_ptr) {
    *new_handle_ptr = duplicated.value_or(X_INVALID_HANDLE_VALUE);
  }

  if (options == 1 /* DUPLICATE_CLOSE_SOURCE */) {
    (void)table.Remove(handle);
  }

  return ToXStatus(duplicated);
}

u32 NtClose_entry(u32 handle) {
  REXLOG_TRACE(kernel,
               "NtClose "
               "handle={:#x}",
               (uint32_t)handle);
  auto result = ToXStatus(runtime::Current().object_table().Close(handle));
  REXLOG_TRACE(kernel,
               "NtClose -> "
               "{:#x}",
               result);
  return result;
}

u32 NtQueryEvent_entry(u32 handle, mapped_u32 out_struc) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = runtime::Current().object_table().Get<EventObject>(handle);
  if (ev) {
    uint32_t type_tmp, state_tmp;
    EventQuery(*ev, &type_tmp, &state_tmp);
    out_struc[0] = type_tmp;
    out_struc[1] = state_tmp;
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
