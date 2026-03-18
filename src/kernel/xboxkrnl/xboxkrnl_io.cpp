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

#include <rex/filesystem/device.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/info/file.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

struct CreateOptions {
  // https://processhacker.sourceforge.io/doc/ntioapi_8h.html
  static const uint32_t FILE_DIRECTORY_FILE = 0x00000001;
  // Optimization - files access will be sequential, not random.
  static const uint32_t FILE_SEQUENTIAL_ONLY = 0x00000004;
  static const uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
  static const uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
  static const uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
  // Optimization - file access will be random, not sequential.
  static const uint32_t FILE_RANDOM_ACCESS = 0x00000800;
};

static bool IsValidPath(const std::string_view s, bool is_pattern) {
  // TODO(gibbed): validate path components individually
  bool got_asterisk = false;
  for (const auto& c : s) {
    if (c <= 31 || c >= 127) {
      return false;
    }
    if (got_asterisk) {
      // * must be followed by a . (*.)
      //
      // 4D530819 has a bug in its game code where it attempts to
      // FindFirstFile() with filters of "Game:\\*_X3.rkv", "Game:\\m*_X3.rkv",
      // and "Game:\\w*_X3.rkv" and will infinite loop if the path filter is
      // allowed.
      if (c != '.') {
        return false;
      }
      got_asterisk = false;
    }
    switch (c) {
      case '"':
      // case '*':
      case '+':
      case ',':
      // case ':':
      // case ';':
      case '<':
      // case '=':
      case '>':
      // case '?':
      case '|': {
        return false;
      }
      case '*': {
        // Pattern-specific (for NtQueryDirectoryFile)
        if (!is_pattern) {
          return false;
        }
        got_asterisk = true;
        break;
      }
      case '?': {
        // Pattern-specific (for NtQueryDirectoryFile)
        if (!is_pattern) {
          return false;
        }
        break;
      }
      default: {
        break;
      }
    }
  }
  return true;
}

ppc_u32_result_t NtCreateFile_entry(ppc_pu32_t handle_out, ppc_u32_t desired_access,
                                    ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs,
                                    ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                    ppc_pu64_t allocation_size_ptr, ppc_u32_t file_attributes,
                                    ppc_u32_t share_access, ppc_u32_t creation_disposition,
                                    ppc_u32_t create_options) {
  // note used. maybe later
  // uint64_t allocation_size = 0;  // is this correct???
  // if (allocation_size_ptr) {
  //  allocation_size = *allocation_size_ptr;
  //}

  if (!object_attrs) {
    // ..? Some games do this. This parameter is not optional.
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  rex::filesystem::Entry* root_entry = nullptr;

  // Compute path, possibly attrs relative.
  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXKRNL_IMPORT_TRACE(
      "NtCreateFile", "path={} access={:#x} attrs={:#x} share={:#x} disp={:#x} options={:#x}",
      target_path, (uint32_t)desired_access, (uint32_t)file_attributes, (uint32_t)share_access,
      (uint32_t)creation_disposition, (uint32_t)create_options);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      object_attrs->root_directory != 0) {
    auto root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(object_attrs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);

    root_entry = root_file->entry();
  }

  // Attempt open (or create).
  rex::filesystem::File* vfs_file;
  rex::filesystem::FileAction file_action;
  X_STATUS result = REX_KERNEL_FS()->OpenFile(
      root_entry, target_path, rex::filesystem::FileDisposition((uint32_t)creation_disposition),
      desired_access, (create_options & CreateOptions::FILE_DIRECTORY_FILE) != 0,
      (create_options & CreateOptions::FILE_NON_DIRECTORY_FILE) != 0, &vfs_file, &file_action);
  object_ref<XFile> file = nullptr;

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XSUCCEEDED(result)) {
    // If true, desired_access SYNCHRONIZE flag must be set.
    bool synchronous = (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_ALERT) ||
                       (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_NONALERT);
    file = object_ref<XFile>(new XFile(REX_KERNEL_STATE(), vfs_file, synchronous));

    // Handle ref is incremented, so return that.
    handle = file->handle();
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = (uint32_t)file_action;
  }

  *handle_out = handle;
  if (XFAILED(result)) {
    REXKRNL_IMPORT_FAIL("NtCreateFile", "path='{}' -> {:#x}", target_path, result);
  } else {
    REXKRNL_IMPORT_RESULT("NtCreateFile", "{:#x} handle={:#x}", result, handle);
  }
  return result;
}

ppc_u32_result_t NtOpenFile_entry(ppc_pu32_t handle_out, ppc_u32_t desired_access,
                                  ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attributes,
                                  ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                  ppc_u32_t open_options) {
  return NtCreateFile_entry(handle_out, desired_access, object_attributes, io_status_block, nullptr,
                            0, 0, static_cast<uint32_t>(rex::filesystem::FileDisposition::kOpen),
                            open_options);
}

ppc_u32_result_t NtReadFile_entry(ppc_u32_t file_handle, ppc_u32_t event_handle,
                                  ppc_pvoid_t apc_routine_ptr, ppc_pvoid_t apc_context,
                                  ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, ppc_pvoid_t buffer,
                                  ppc_u32_t buffer_length, ppc_pu64_t byte_offset_ptr) {
  uint64_t byte_offset = byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : 0;
  const bool apc_requested = (static_cast<uint32_t>(apc_routine_ptr) & ~1u) != 0;
  REXKRNL_IMPORT_TRACE(
      "NtReadFile",
      "handle={:#x} event={:#x} apc={:#x} apc_ctx={:#x} iosb={:#x} buf={:#x} len={:#x} offset={}",
      (uint32_t)file_handle, (uint32_t)event_handle, apc_routine_ptr.guest_address(),
      apc_context.guest_address(), io_status_block.guest_address(), buffer.guest_address(),
      (uint32_t)buffer_length, byte_offset_ptr ? (int64_t)byte_offset : -1);
  X_STATUS result = X_STATUS_SUCCESS;
  bool apc_queued = false;

  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous()) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = file->Read(buffer.guest_address(), buffer_length,
                          byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                          &bytes_read, apc_context.guest_address());
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context && result == X_STATUS_SUCCESS) {
          auto thread = XThread::GetCurrentThread();
          uint32_t apc_routine = static_cast<uint32_t>(apc_routine_ptr) & ~1u;
          uint32_t apc_ctx = apc_context.guest_address();
          uint32_t apc_arg1 = io_status_block.guest_address();
          REXKRNL_IMPORT_TRACE("NtReadFile",
                               "queue_apc thid={} normal={:#x} ctx={:#x} arg1={:#x} arg2=0",
                               thread ? thread->thread_id() : 0, apc_routine, apc_ctx, apc_arg1);
          thread->EnqueueApc(apc_routine, apc_ctx, apc_arg1, 0);
          apc_queued = true;
        } else {
          REXKRNL_IMPORT_TRACE("NtReadFile", "skip_apc_queue (apc_ctx={:#x}, status={:#x})",
                               apc_context.guest_address(), result);
        }
      }

      if (!file->is_synchronous() && result != X_STATUS_END_OF_FILE) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // TODO(benvanik): async.

      // X_STATUS_PENDING if not returning immediately.
      // XFile is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
      /*xeNtReadFileState* call_state = new xeNtReadFileState();
      XAsyncRequest* request = new XAsyncRequest(
      state, file,
      (XAsyncRequest::CompletionCallback)xeNtReadFileCompleted,
      call_state);*/
      // result = file->Read(buffer.guest_address(), buffer_length, byte_offset,
      //                     request);
      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }

      result = X_STATUS_PENDING;
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  // Log detailed completion info for debugging async IO issues
  if (file) {
    REXKRNL_IMPORT_RESULT(
        "NtReadFile",
        "{:#x} (sync={}, iosb_status={:#x}, iosb_info={}, ev_signaled={}, apc_requested={}, "
        "apc_queued={})",
        result, file->is_synchronous(),
        io_status_block ? (uint32_t)io_status_block->status : 0xDEAD,
        io_status_block ? (uint32_t)io_status_block->information : 0, ev && signal_event,
        apc_requested, apc_queued);
  } else {
    REXKRNL_IMPORT_RESULT("NtReadFile", "{:#x} (apc_requested={}, apc_queued={})", result,
                          apc_requested, apc_queued);
  }
  return result;
}

ppc_u32_result_t NtReadFileScatter_entry(ppc_u32_t file_handle, ppc_u32_t event_handle,
                                         ppc_pvoid_t apc_routine_ptr, ppc_pvoid_t apc_context,
                                         ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                         ppc_pu32_t segment_array, ppc_u32_t length,
                                         ppc_pu64_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous()) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = file->ReadScatter(segment_array.guest_address(), length,
                                 byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                                 &bytes_read, apc_context.guest_address());
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine_ptr) & ~1u,
                             apc_context.guest_address(), io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // TODO(benvanik): async.

      // TODO: On Windows it might be worth trying to use Win32 ReadFileScatter
      // here instead of handling it ourselves

      // X_STATUS_PENDING if not returning immediately.
      // XFile is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
      /*xeNtReadFileState* call_state = new xeNtReadFileState();
      XAsyncRequest* request = new XAsyncRequest(
      state, file,
      (XAsyncRequest::CompletionCallback)xeNtReadFileCompleted,
      call_state);*/
      // result = file->Read(buffer.guest_address(), buffer_length, byte_offset,
      //                     request);
      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }

      result = X_STATUS_PENDING;
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}

ppc_u32_result_t NtWriteFile_entry(ppc_u32_t file_handle, ppc_u32_t event_handle,
                                   ppc_fn_t apc_routine, ppc_pvoid_t apc_context,
                                   ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, ppc_pvoid_t buffer,
                                   ppc_u32_t buffer_length, ppc_pu64_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  // Grab event to signal.
  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Grab file.
  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Execute write.
  if (XSUCCEEDED(result)) {
    // TODO(benvanik): async path.
    if (true || file->is_synchronous()) {
      // Synchronous request.
      uint32_t bytes_written = 0;
      result = file->Write(buffer.guest_address(), buffer_length,
                           byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                           &bytes_written, apc_context.guest_address());

      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = static_cast<uint32_t>(bytes_written);
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine) & ~1u, apc_context.guest_address(),
                             io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // X_STATUS_PENDING if not returning immediately.
      result = X_STATUS_PENDING;

      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}

ppc_u32_result_t NtCreateIoCompletion_entry(ppc_pu32_t out_handle, ppc_u32_t desired_access,
                                            ppc_pvoid_t object_attribs,
                                            ppc_u32_t num_concurrent_threads) {
  auto completion = new XIOCompletion(REX_KERNEL_STATE());
  if (out_handle) {
    *out_handle = completion->handle();
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t NtSetIoCompletion_entry(ppc_u32_t handle, ppc_u32_t key_context,
                                         ppc_u32_t apc_context, ppc_u32_t completion_status,
                                         ppc_u32_t num_bytes) {
  auto port = REX_KERNEL_OBJECTS()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    return X_STATUS_INVALID_HANDLE;
  }

  XIOCompletion::IONotification notification;
  notification.key_context = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  port->QueueNotification(notification);
  return X_STATUS_SUCCESS;
}

// Dequeues a packet from the completion port.
ppc_u32_result_t NtRemoveIoCompletion_entry(ppc_u32_t handle, ppc_pu32_t key_context,
                                            ppc_pu32_t apc_context,
                                            ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                            ppc_pu64_t timeout) {
  X_STATUS status = X_STATUS_SUCCESS;
  // uint32_t info = 0;

  auto port = REX_KERNEL_OBJECTS()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    status = X_STATUS_INVALID_HANDLE;
  }

  uint64_t timeout_ticks = timeout ? static_cast<uint32_t>(*timeout)
                                   : static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  XIOCompletion::IONotification notification;
  if (port->WaitForNotification(timeout_ticks, &notification)) {
    if (key_context) {
      *key_context = notification.key_context;
    }
    if (apc_context) {
      *apc_context = notification.apc_context;
    }

    if (io_status_block) {
      io_status_block->status = notification.status;
      io_status_block->information = notification.num_bytes;
    }
  } else {
    status = X_STATUS_TIMEOUT;
  }

  return status;
}

ppc_u32_result_t NtQueryFullAttributesFile_entry(
    ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attribs,
    ppc_ptr_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(obj_attribs->name_ptr);
  auto path_str = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXKRNL_IMPORT_TRACE("NtQueryFullAttributesFile", "path={}", path_str);

  object_ref<XFile> root_file;
  if (obj_attribs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      obj_attribs->root_directory != 0) {
    root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(obj_attribs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);
    assert_always();
  }

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Resolve the file using the virtual file system.
  auto entry = REX_KERNEL_FS()->ResolvePath(target_path);
  if (entry) {
    // Found.
    file_info->creation_time = entry->create_timestamp();
    file_info->last_access_time = entry->access_timestamp();
    file_info->last_write_time = entry->write_timestamp();
    file_info->change_time = entry->write_timestamp();
    file_info->allocation_size = entry->allocation_size();
    file_info->end_of_file = entry->size();
    file_info->attributes = entry->attributes();

    REXKRNL_IMPORT_RESULT("NtQueryFullAttributesFile", "0x0 size={}", (uint64_t)entry->size());
    return X_STATUS_SUCCESS;
  }

  REXKRNL_IMPORT_RESULT("NtQueryFullAttributesFile", "X_STATUS_NO_SUCH_FILE");
  return X_STATUS_NO_SUCH_FILE;
}

ppc_u32_result_t NtQueryDirectoryFile_entry(ppc_u32_t file_handle, ppc_u32_t event_handle,
                                            ppc_fn_t apc_routine, ppc_pvoid_t apc_context,
                                            ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                            ppc_ptr_t<X_FILE_DIRECTORY_INFORMATION> file_info_ptr,
                                            ppc_u32_t length, ppc_ptr_t<X_ANSI_STRING> file_name,
                                            ppc_u32_t restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  uint32_t info = 0;

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  auto name = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), file_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(name, true)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    // X_FILE_DIRECTORY_INFORMATION dir_info = {0};
    result = file->QueryDirectory(file_info_ptr, length, name, restart_scan != 0);
    if (XSUCCEEDED(result)) {
      info = length;
    }
  } else {
    result = X_STATUS_NO_SUCH_FILE;
  }

  if (XFAILED(result)) {
    info = 0;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = info;
  }

  return result;
}

ppc_u32_result_t NtFlushBuffersFile_entry(ppc_u32_t file_handle,
                                          ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block_ptr) {
  auto result = X_STATUS_SUCCESS;

  if (io_status_block_ptr) {
    io_status_block_ptr->status = result;
    io_status_block_ptr->information = 0;
  }

  return result;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntopensymboliclinkobject
ppc_u32_result_t NtOpenSymbolicLinkObject_entry(ppc_pu32_t handle_out,
                                                ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  if (!object_attrs) {
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  assert_true(object_attrs->attributes == 64);  // case insensitive

  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0) {
    assert_always();
  }

  if (rex::string::utf8_starts_with(target_path, "\\??\\")) {
    target_path = target_path.substr(4);  // Strip the full qualifier
  }

  std::string link_path;
  if (!REX_KERNEL_FS()->FindSymbolicLink(target_path, link_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }

  object_ref<XSymbolicLink> symlink(new XSymbolicLink(REX_KERNEL_STATE()));
  symlink->Initialize(target_path, link_path);

  *handle_out = symlink->handle();

  return X_STATUS_SUCCESS;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntquerysymboliclinkobject
ppc_u32_result_t NtQuerySymbolicLinkObject_entry(ppc_u32_t handle,
                                                 ppc_ptr_t<X_ANSI_STRING> target) {
  auto symlink = REX_KERNEL_OBJECTS()->LookupObject<XSymbolicLink>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length), symlink->target().size());
  if (length > 0) {
    auto target_buf = REX_KERNEL_MEMORY()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target().c_str(), length);
  }
  target->length = static_cast<uint16_t>(length);
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t FscGetCacheElementCount_entry(ppc_u32_t r3) {
  return 0;
}

ppc_u32_result_t FscSetCacheElementCount_entry(ppc_u32_t unk_0, ppc_u32_t unk_1) {
  // unk_0 = 0
  // unk_1 looks like a count? in what units? 256 is a common value
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t NtDeviceIoControlFile_entry(ppc_u32_t handle, ppc_u32_t event_handle,
                                             ppc_u32_t apc_routine, ppc_u32_t apc_context,
                                             ppc_u32_t io_status_block, ppc_u32_t io_control_code,
                                             ppc_pvoid_t input_buffer, ppc_u32_t input_buffer_len,
                                             ppc_pvoid_t output_buffer,
                                             ppc_u32_t output_buffer_len) {
  // Called by XMountUtilityDrive cache-mounting code
  // (checks if the returned values look valid, values below seem to pass the
  // checks)
  const uint32_t cache_size = 0xFF000;

  const uint32_t X_IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000;
  const uint32_t X_IOCTL_DISK_GET_PARTITION_INFO = 0x74004;

  if (io_control_code == X_IOCTL_DISK_GET_DRIVE_GEOMETRY) {
    if (output_buffer_len < 0x8) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    memory::store_and_swap<uint32_t>(output_buffer, cache_size / 512);
    memory::store_and_swap<uint32_t>(output_buffer + 4, 512);
  } else if (io_control_code == X_IOCTL_DISK_GET_PARTITION_INFO) {
    if (output_buffer_len < 0x10) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    memory::store_and_swap<uint64_t>(output_buffer, 0);
    memory::store_and_swap<uint64_t>(output_buffer + 8, cache_size);
  } else {
    REXKRNL_DEBUG("NtDeviceIoControlFile(0x{:X}) - unhandled IOCTL!", uint32_t(io_control_code));
    assert_always();
    return X_STATUS_INVALID_PARAMETER;
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t IoCreateDevice_entry(ppc_u32_t device_struct, ppc_u32_t r4, ppc_u32_t r5,
                                      ppc_u32_t r6, ppc_u32_t r7, ppc_pu32_t out_struct) {
  // Called from XMountUtilityDrive XAM-task code
  // That code tries writing things to a pointer at out_struct+0x18
  // We'll alloc some scratch space for it so it doesn't cause any exceptions

  // 0x24 is guessed size from accesses to out_struct - likely incorrect
  auto out_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x24);

  auto out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(out_guest);
  memset(out, 0, 0x24);

  // XMountUtilityDrive writes some kind of header here
  // 0x1000 bytes should be enough to store it
  auto out_guest2 = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x1000);
  memory::store_and_swap(out + 0x18, out_guest2);

  *out_struct = out_guest;
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t IoDismountVolumeByFileHandle_entry(ppc_u32_t handle) {
  REXKRNL_WARN("IoDismountVolumeByFileHandle({:#x}) - stub", (uint32_t)handle);
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t IoDismountVolumeByName_entry(ppc_ptr_t<X_ANSI_STRING> name) {
  REXKRNL_WARN("IoDismountVolumeByName - stub");
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t IoSynchronousDeviceIoControlRequest_entry(
    ppc_u32_t ioctl, ppc_pvoid_t device_object, ppc_pvoid_t input_buffer, ppc_u32_t input_length,
    ppc_pvoid_t output_buffer, ppc_u32_t output_length, ppc_pu32_t returned_length, ppc_u32_t unk) {
  REXKRNL_WARN("IoSynchronousDeviceIoControlRequest({:#x}) - stub", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t StfsCreateDevice_entry(ppc_pvoid_t device_object, ppc_u32_t flags,
                                        ppc_pu32_t out_device) {
  REXKRNL_WARN("StfsCreateDevice - stub");
  // if (out_device) *out_device = 0;
  return X_STATUS_SUCCESS;
}

ppc_u32_result_t StfsControlDevice_entry(ppc_pvoid_t device_object, ppc_u32_t ioctl,
                                         ppc_pvoid_t input_buffer, ppc_u32_t input_length,
                                         ppc_pvoid_t output_buffer, ppc_u32_t output_length) {
  REXKRNL_WARN("StfsControlDevice({:#x}) - stub", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__NtCreateFile, rex::kernel::xboxkrnl::NtCreateFile_entry)
XBOXKRNL_EXPORT(__imp__NtOpenFile, rex::kernel::xboxkrnl::NtOpenFile_entry)
XBOXKRNL_EXPORT(__imp__NtReadFile, rex::kernel::xboxkrnl::NtReadFile_entry)
XBOXKRNL_EXPORT(__imp__NtReadFileScatter, rex::kernel::xboxkrnl::NtReadFileScatter_entry)
XBOXKRNL_EXPORT(__imp__NtWriteFile, rex::kernel::xboxkrnl::NtWriteFile_entry)
XBOXKRNL_EXPORT(__imp__NtCreateIoCompletion, rex::kernel::xboxkrnl::NtCreateIoCompletion_entry)
XBOXKRNL_EXPORT(__imp__NtSetIoCompletion, rex::kernel::xboxkrnl::NtSetIoCompletion_entry)
XBOXKRNL_EXPORT(__imp__NtRemoveIoCompletion, rex::kernel::xboxkrnl::NtRemoveIoCompletion_entry)
XBOXKRNL_EXPORT(__imp__NtQueryFullAttributesFile,
                rex::kernel::xboxkrnl::NtQueryFullAttributesFile_entry)
XBOXKRNL_EXPORT(__imp__NtQueryDirectoryFile, rex::kernel::xboxkrnl::NtQueryDirectoryFile_entry)
XBOXKRNL_EXPORT(__imp__NtFlushBuffersFile, rex::kernel::xboxkrnl::NtFlushBuffersFile_entry)
XBOXKRNL_EXPORT(__imp__NtOpenSymbolicLinkObject,
                rex::kernel::xboxkrnl::NtOpenSymbolicLinkObject_entry)
XBOXKRNL_EXPORT(__imp__NtQuerySymbolicLinkObject,
                rex::kernel::xboxkrnl::NtQuerySymbolicLinkObject_entry)
XBOXKRNL_EXPORT(__imp__FscGetCacheElementCount,
                rex::kernel::xboxkrnl::FscGetCacheElementCount_entry)
XBOXKRNL_EXPORT(__imp__FscSetCacheElementCount,
                rex::kernel::xboxkrnl::FscSetCacheElementCount_entry)
XBOXKRNL_EXPORT(__imp__NtDeviceIoControlFile, rex::kernel::xboxkrnl::NtDeviceIoControlFile_entry)
XBOXKRNL_EXPORT(__imp__IoCreateDevice, rex::kernel::xboxkrnl::IoCreateDevice_entry)
XBOXKRNL_EXPORT(__imp__IoDismountVolumeByFileHandle,
                rex::kernel::xboxkrnl::IoDismountVolumeByFileHandle_entry)
XBOXKRNL_EXPORT(__imp__IoDismountVolumeByName, rex::kernel::xboxkrnl::IoDismountVolumeByName_entry)
XBOXKRNL_EXPORT(__imp__IoSynchronousDeviceIoControlRequest,
                rex::kernel::xboxkrnl::IoSynchronousDeviceIoControlRequest_entry)
XBOXKRNL_EXPORT(__imp__StfsCreateDevice, rex::kernel::xboxkrnl::StfsCreateDevice_entry)
XBOXKRNL_EXPORT(__imp__StfsControlDevice, rex::kernel::xboxkrnl::StfsControlDevice_entry)

XBOXKRNL_EXPORT_STUB(__imp__IoAcquireDeviceObjectLock);
XBOXKRNL_EXPORT_STUB(__imp__IoAllocateIrp);
XBOXKRNL_EXPORT_STUB(__imp__IoBuildAsynchronousFsdRequest);
XBOXKRNL_EXPORT_STUB(__imp__IoBuildDeviceIoControlRequest);
XBOXKRNL_EXPORT_STUB(__imp__IoBuildSynchronousFsdRequest);
XBOXKRNL_EXPORT_STUB(__imp__IoCallDriver);
XBOXKRNL_EXPORT_STUB(__imp__IoCheckShareAccess);
XBOXKRNL_EXPORT_STUB(__imp__IoCompleteRequest);
XBOXKRNL_EXPORT_STUB(__imp__IoCreateFile);
XBOXKRNL_EXPORT_STUB(__imp__IoDeleteDevice);
XBOXKRNL_EXPORT_STUB(__imp__IoDismountVolume);
XBOXKRNL_EXPORT_STUB(__imp__IoFreeIrp);
XBOXKRNL_EXPORT_STUB(__imp__IoInitializeIrp);
XBOXKRNL_EXPORT_STUB(__imp__IoInvalidDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__IoQueueThreadIrp);
XBOXKRNL_EXPORT_STUB(__imp__IoReleaseDeviceObjectLock);
XBOXKRNL_EXPORT_STUB(__imp__IoRemoveShareAccess);
XBOXKRNL_EXPORT_STUB(__imp__IoSetIoCompletion);
XBOXKRNL_EXPORT_STUB(__imp__IoSetShareAccess);
XBOXKRNL_EXPORT_STUB(__imp__IoStartNextPacket);
XBOXKRNL_EXPORT_STUB(__imp__IoStartNextPacketByKey);
XBOXKRNL_EXPORT_STUB(__imp__IoStartPacket);
XBOXKRNL_EXPORT_STUB(__imp__IoSynchronousFsdRequest);
XBOXKRNL_EXPORT_STUB(__imp__NtDeleteFile);
XBOXKRNL_EXPORT_STUB(__imp__NtSetSystemTime);
XBOXKRNL_EXPORT_STUB(__imp__NtWriteFileGather);
XBOXKRNL_EXPORT_STUB(__imp__IoAcquireCancelSpinLock);
XBOXKRNL_EXPORT_STUB(__imp__IoReleaseCancelSpinLock);
XBOXKRNL_EXPORT_STUB(__imp__NtCancelIoFile);
XBOXKRNL_EXPORT_STUB(__imp__NtCancelIoFileEx);
XBOXKRNL_EXPORT_STUB(__imp__SvodCreateDevice);

XBOXKRNL_EXPORT_STUB(__imp__SataCdRomRecordReset);
