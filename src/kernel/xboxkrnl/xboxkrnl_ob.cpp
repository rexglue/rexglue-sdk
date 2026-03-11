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

#include <rex/assert.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xobject.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

ppc_u32_result_t ObOpenObjectByName_entry(ppc_pvoid_t obj_attributes_ptr,
                                          ppc_pvoid_t object_type_ptr, ppc_u32_t unk,
                                          ppc_pu32_t handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  if (!obj_attributes_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto obj_attributes =
      kernel_memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(obj_attributes_ptr.guest_address());
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_memory(), obj_attributes->name_ptr);

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result = kernel_state()->object_table()->GetObjectByName(name, &handle);
  if (XSUCCEEDED(result)) {
    *handle_ptr = handle;
  }

  return result;
}

ppc_u32_result_t ObOpenObjectByPointer_entry(ppc_pvoid_t object_ptr, ppc_pu32_t out_handle_ptr) {
  auto object = XObject::GetNativeObject<XObject>(kernel_state(), object_ptr);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Retain the handle. Will be released in NtClose.
  object->RetainHandle();
  *out_handle_ptr = object->handle();
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t ObLookupThreadByThreadId_entry(ppc_u32_t thread_id, ppc_pu32_t out_object_ptr) {
  auto thread = kernel_state()->GetThreadByID(thread_id);
  if (!thread) {
    return X_STATUS_NOT_FOUND;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  thread->RetainHandle();
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t ObReferenceObjectByHandle_entry(ppc_u32_t handle, ppc_u32_t object_type_ptr,
                                                 ppc_pu32_t out_object_ptr) {
  REXKRNL_IMPORT_TRACE("ObReferenceObjectByHandle", "handle={:#x} type={:#x}", (uint32_t)handle,
                       (uint32_t)object_type_ptr);

  object_ref<XObject> object;

  // Handle pseudo-handles.
  uint32_t handle_val = static_cast<uint32_t>(handle);
  if (handle_val == 0xFFFFFFFE) {
    // CurrentThread pseudo-handle.
    auto thread = XThread::GetCurrentThread();
    if (!thread) {
      return X_STATUS_INVALID_HANDLE;
    }
    object = retain_object(static_cast<XObject*>(thread));
  } else {
    object = kernel_state()->object_table()->LookupObject<XObject>(handle);
  }

  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t native_ptr = object->guest_object();

  // Type check using real KernelGuestGlobals addresses.
  if (object_type_ptr) {
    uint32_t globals_base = kernel_state()->GetKernelGuestGlobals();
    uint32_t expected_type = 0;
    switch (object->type()) {
      case XObject::Type::Thread:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExThreadObjectType);
        break;
      case XObject::Type::Event:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExEventObjectType);
        break;
      case XObject::Type::Mutant:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExMutantObjectType);
        break;
      case XObject::Type::Semaphore:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExSemaphoreObjectType);
        break;
      case XObject::Type::Timer:
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
  object->RetainHandle();
  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }
  REXKRNL_IMPORT_RESULT("ObReferenceObjectByHandle", "0x0 obj={:#x}", native_ptr);
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t ObReferenceObjectByName_entry(ppc_pchar_t name, ppc_u32_t attributes,
                                               ppc_u32_t object_type_ptr, ppc_pvoid_t parse_context,
                                               ppc_pu32_t out_object_ptr) {
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result = kernel_state()->object_table()->GetObjectByName(name.value(), &handle);
  if (XSUCCEEDED(result)) {
    return ObReferenceObjectByHandle_entry(handle, object_type_ptr, out_object_ptr);
  }

  return result;
}

ppc_u32_result_t ObDereferenceObject_entry(ppc_u32_t native_ptr) {
  REXKRNL_IMPORT_TRACE("ObDereferenceObject", "ptr={:#x}", (uint32_t)native_ptr);
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return 0;
  }

  auto object = XObject::GetNativeObject<XObject>(kernel_state(),
                                                  kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->ReleaseHandle();
  }

  return 0;
}

ppc_u32_result_t ObCreateSymbolicLink_entry(ppc_ptr_t<X_ANSI_STRING> path_ptr,
                                            ppc_ptr_t<X_ANSI_STRING> target_ptr) {
  auto path = rex::string::utf8_canonicalize_guest_path(
      util::TranslateAnsiString(kernel_memory(), path_ptr));
  auto target = rex::string::utf8_canonicalize_guest_path(
      util::TranslateAnsiString(kernel_memory(), target_ptr));

  if (rex::string::utf8_starts_with(path, u8"\\??\\")) {
    path = path.substr(4);  // Strip the full qualifier
  }

  if (!kernel_state()->file_system()->RegisterSymbolicLink(path, target)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t ObDeleteSymbolicLink_entry(ppc_ptr_t<X_ANSI_STRING> path_ptr) {
  auto path = util::TranslateAnsiString(kernel_memory(), path_ptr);
  if (!kernel_state()->file_system()->UnregisterSymbolicLink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t NtDuplicateObject_entry(ppc_u32_t handle, ppc_pu32_t new_handle_ptr,
                                         ppc_u32_t options) {
  // NOTE: new_handle_ptr can be zero to just close a handle.
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // This function actually just creates a new handle to the same object.
  // Most games use it to get real handles to the current thread or whatever.

  X_HANDLE new_handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result = kernel_state()->object_table()->DuplicateHandle(handle, &new_handle);

  if (new_handle_ptr) {
    *new_handle_ptr = new_handle;
  }

  if (options == 1 /* DUPLICATE_CLOSE_SOURCE */) {
    // Always close the source object.
    kernel_state()->object_table()->RemoveHandle(handle);
  }

  return result;
}

ppc_u32_result_t NtClose_entry(ppc_u32_t handle) {
  REXKRNL_IMPORT_TRACE("NtClose", "handle={:#x}", (uint32_t)handle);
  auto result = kernel_state()->object_table()->ReleaseHandle(handle);
  REXKRNL_IMPORT_RESULT("NtClose", "{:#x}", result);
  return result;
}

ppc_u32_result_t NtQueryEvent_entry(ppc_u32_t handle, ppc_pu32_t out_struc) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    uint32_t type_tmp, state_tmp;
    ev->Query(&type_tmp, &state_tmp);
    out_struc[0] = type_tmp;
    out_struc[1] = state_tmp;
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__ObOpenObjectByName, rex::kernel::xboxkrnl::ObOpenObjectByName_entry)
XBOXKRNL_EXPORT(__imp__ObOpenObjectByPointer, rex::kernel::xboxkrnl::ObOpenObjectByPointer_entry)
XBOXKRNL_EXPORT(__imp__ObLookupThreadByThreadId,
                rex::kernel::xboxkrnl::ObLookupThreadByThreadId_entry)
XBOXKRNL_EXPORT(__imp__ObReferenceObjectByHandle,
                rex::kernel::xboxkrnl::ObReferenceObjectByHandle_entry)
XBOXKRNL_EXPORT(__imp__ObReferenceObjectByName,
                rex::kernel::xboxkrnl::ObReferenceObjectByName_entry)
XBOXKRNL_EXPORT(__imp__ObDereferenceObject, rex::kernel::xboxkrnl::ObDereferenceObject_entry)
XBOXKRNL_EXPORT(__imp__ObCreateSymbolicLink, rex::kernel::xboxkrnl::ObCreateSymbolicLink_entry)
XBOXKRNL_EXPORT(__imp__ObDeleteSymbolicLink, rex::kernel::xboxkrnl::ObDeleteSymbolicLink_entry)
XBOXKRNL_EXPORT(__imp__NtDuplicateObject, rex::kernel::xboxkrnl::NtDuplicateObject_entry)
XBOXKRNL_EXPORT(__imp__NtClose, rex::kernel::xboxkrnl::NtClose_entry)
XBOXKRNL_EXPORT(__imp__NtQueryEvent, rex::kernel::xboxkrnl::NtQueryEvent_entry)

XBOXKRNL_EXPORT_STUB(__imp__ObCreateObject);
XBOXKRNL_EXPORT_STUB(__imp__ObGetWaitableObject);
XBOXKRNL_EXPORT_STUB(__imp__ObInsertObject);
XBOXKRNL_EXPORT_STUB(__imp__ObIsTitleObject);
XBOXKRNL_EXPORT_STUB(__imp__ObLookupAnyThreadByThreadId);
XBOXKRNL_EXPORT_STUB(__imp__ObMakeTemporaryObject);
XBOXKRNL_EXPORT_STUB(__imp__ObReferenceObject);
XBOXKRNL_EXPORT_STUB(__imp__ObTranslateSymbolicLink);
XBOXKRNL_EXPORT_STUB(__imp__NtCreateDirectoryObject);
XBOXKRNL_EXPORT_STUB(__imp__NtCreateSymbolicLinkObject);
XBOXKRNL_EXPORT_STUB(__imp__NtMakeTemporaryObject);
XBOXKRNL_EXPORT_STUB(__imp__NtOpenDirectoryObject);
XBOXKRNL_EXPORT_STUB(__imp__NtQueryDirectoryObject);
XBOXKRNL_EXPORT_STUB(__imp__NtQueryIoCompletion);
XBOXKRNL_EXPORT_STUB(__imp__NtQueryMutant);
XBOXKRNL_EXPORT_STUB(__imp__NtQuerySemaphore);
XBOXKRNL_EXPORT_STUB(__imp__NtQueryTimer);
