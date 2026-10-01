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

#include <rex/filesystem.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/info/file.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include "kernel/kernel_macros.h"

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

u32 NtCreateFile_entry(mapped_u32 handle_out, u32 desired_access,
                       ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs,
                       ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 allocation_size_ptr,
                       u32 file_attributes, u32 share_access, u32 creation_disposition,
                       u32 create_options) {
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

  // Compute path, possibly attrs relative.
  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXLOG_TRACE(kernel,
               "NtCreateFile "
               "path={} access={:#x} attrs={:#x} share={:#x} disp={:#x} options={:#x}",
               target_path, (uint32_t)desired_access, (uint32_t)file_attributes,
               (uint32_t)share_access, (uint32_t)creation_disposition, (uint32_t)create_options);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  Pinned<FileObject> root_file;
  if (object_attrs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      object_attrs->root_directory != 0) {
    root_file = REX_KERNEL_OBJECTS()->Get<FileObject>(object_attrs->root_directory);
    assert_true(static_cast<bool>(root_file));
  }

  filesystem::OpenOptions options;
  options.disposition =
      static_cast<filesystem::Disposition>(static_cast<uint32_t>(creation_disposition));
  options.access = static_cast<filesystem::Access>(static_cast<uint32_t>(desired_access));
  options.create_directory = (create_options & CreateOptions::FILE_DIRECTORY_FILE) != 0;
  options.require_file = (create_options & CreateOptions::FILE_NON_DIRECTORY_FILE) != 0;
  auto opened = root_file ? REX_KERNEL_FS()->OpenRelative(*root_file->file, target_path, options)
                          : REX_KERNEL_FS()->Open(target_path, options);
  X_STATUS result = X_STATUS_SUCCESS;
  filesystem::OpenAction file_action = filesystem::OpenAction::kDoesNotExist;
  if (opened) {
    file_action = opened->action;
  } else {
    result = ToXStatus(opened.error());
    if (filesystem::CodeOf(opened.error()) == filesystem::Error::kExists) {
      file_action = filesystem::OpenAction::kExists;
    }
  }
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XSUCCEEDED(result)) {
    // If true, desired_access SYNCHRONIZE flag must be set.
    bool synchronous = (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_ALERT) ||
                       (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_NONALERT);
    result = REX_KERNEL_OBJECTS()->Emplace<FileObject>(&handle, AddOptions{},
                                                       std::move(opened->file), synchronous);
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = (uint32_t)file_action;
  }

  *handle_out = handle;
  if (XFAILED(result)) {
    REXLOG_WARN(kernel,
                "NtCreateFile failed "
                "path='{}' -> {:#x}",
                target_path, result);
  } else {
    REXLOG_TRACE(kernel,
                 "NtCreateFile -> "
                 "{:#x} handle={:#x}",
                 result, handle);
  }
  return result;
}

u32 NtOpenFile_entry(mapped_u32 handle_out, u32 desired_access,
                     ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attributes,
                     ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 open_options) {
  return NtCreateFile_entry(handle_out, desired_access, object_attributes, io_status_block, nullptr,
                            0, 0, std::to_underlying(filesystem::Disposition::kOpen), open_options);
}

u32 NtReadFile_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                     mapped_void apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                     mapped_void buffer, u32 buffer_length, mapped_u64 byte_offset_ptr) {
  uint64_t byte_offset = byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : 0;
  const bool apc_requested = (static_cast<uint32_t>(apc_routine_ptr) & ~1u) != 0;
  REXLOG_TRACE(
      kernel,
      "NtReadFile "
      "handle={:#x} event={:#x} apc={:#x} apc_ctx={:#x} iosb={:#x} buf={:#x} len={:#x} offset={}",
      (uint32_t)file_handle, (uint32_t)event_handle, apc_routine_ptr.guest_address(),
      apc_context.guest_address(), io_status_block.guest_address(), buffer.guest_address(),
      (uint32_t)buffer_length, byte_offset_ptr ? (int64_t)byte_offset : -1);
  X_STATUS result = X_STATUS_SUCCESS;
  bool apc_queued = false;

  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->Get<EventObject>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = REX_KERNEL_OBJECTS()->Get<FileObject>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = FileRead(REX_KERNEL_STATE(), *file, buffer.guest_address(), buffer_length,
                        byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1, &bytes_read,
                        apc_context.guest_address());
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context && result == X_STATUS_SUCCESS) {
          auto thread = ThreadRecord::GetCurrentThread();
          uint32_t apc_routine = static_cast<uint32_t>(apc_routine_ptr) & ~1u;
          uint32_t apc_ctx = apc_context.guest_address();
          uint32_t apc_arg1 = io_status_block.guest_address();
          REXLOG_TRACE(kernel,
                       "NtReadFile "
                       "queue_apc thid={} normal={:#x} ctx={:#x} arg1={:#x} arg2=0",
                       thread ? thread->thread_id() : 0, apc_routine, apc_ctx, apc_arg1);
          thread->EnqueueApc(apc_routine, apc_ctx, apc_arg1, 0);
          apc_queued = true;
        } else {
          REXLOG_TRACE(kernel,
                       "NtReadFile "
                       "skip_apc_queue (apc_ctx={:#x}, status={:#x})",
                       apc_context.guest_address(), result);
        }
      }

      if (!file->is_synchronous && result != X_STATUS_END_OF_FILE) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // TODO(benvanik): async.

      // X_STATUS_PENDING if not returning immediately.
      // The file is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
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
    EventSet(*ev);
  }

  // Log detailed completion info for debugging async IO issues
  if (file) {
    REXLOG_TRACE(
        kernel,
        "NtReadFile -> "
        "{:#x} (sync={}, iosb_status={:#x}, iosb_info={}, ev_signaled={}, apc_requested={}, "
        "apc_queued={})",
        result, file->is_synchronous, io_status_block ? (uint32_t)io_status_block->status : 0xDEAD,
        io_status_block ? (uint32_t)io_status_block->information : 0, ev && signal_event,
        apc_requested, apc_queued);
  } else {
    REXLOG_TRACE(kernel,
                 "NtReadFile -> "
                 "{:#x} (apc_requested={}, apc_queued={})",
                 result, apc_requested, apc_queued);
  }
  return result;
}

u32 NtReadFileScatter_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                            mapped_void apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                            mapped_u32 segment_array, u32 length, mapped_u64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->Get<EventObject>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = REX_KERNEL_OBJECTS()->Get<FileObject>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = FileReadScatter(REX_KERNEL_STATE(), *file, segment_array.guest_address(), length,
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
          auto thread = ThreadRecord::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine_ptr) & ~1u,
                             apc_context.guest_address(), io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous) {
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
      // The file is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
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
    EventSet(*ev);
  }

  return result;
}

u32 NtWriteFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, mapped_void apc_context,
                      ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_void buffer,
                      u32 buffer_length, mapped_u64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  // Grab event to signal.
  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->Get<EventObject>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Grab file.
  auto file = REX_KERNEL_OBJECTS()->Get<FileObject>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Execute write.
  if (XSUCCEEDED(result)) {
    // TODO(benvanik): async path.
    if (true || file->is_synchronous) {
      // Synchronous request.
      uint32_t bytes_written = 0;
      result = FileWrite(REX_KERNEL_STATE(), *file, buffer.guest_address(), buffer_length,
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
          auto thread = ThreadRecord::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine) & ~1u, apc_context.guest_address(),
                             io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous) {
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
    EventSet(*ev);
  }

  return result;
}

u32 NtCreateIoCompletion_entry(mapped_u32 out_handle, u32 desired_access,
                               mapped_void object_attribs, u32 num_concurrent_threads) {
  X_HANDLE handle = 0;
  X_STATUS status = REX_KERNEL_OBJECTS()->Emplace<IOCompletionObject>(&handle, AddOptions{});
  if (XSUCCEEDED(status) && out_handle) {
    *out_handle = handle;
  }

  return status;
}

u32 NtSetIoCompletion_entry(u32 handle, u32 key_context, u32 apc_context, u32 completion_status,
                            u32 num_bytes) {
  REX_KERNEL_LOOKUP(port, IOCompletionObject, handle);

  IONotification notification;
  notification.key_context = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  IOCompletionQueue(*port, notification);
  return X_STATUS_SUCCESS;
}

// Dequeues a packet from the completion port.
u32 NtRemoveIoCompletion_entry(u32 handle, mapped_u32 key_context, mapped_u32 apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 timeout) {
  X_STATUS status = X_STATUS_SUCCESS;
  // uint32_t info = 0;

  REX_KERNEL_LOOKUP(port, IOCompletionObject, handle);

  uint64_t timeout_ticks = timeout ? static_cast<uint32_t>(*timeout)
                                   : static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  IONotification notification;
  if (IOCompletionWait(*port, timeout_ticks, &notification)) {
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

u32 NtQueryFullAttributesFile_entry(ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attribs,
                                    ppc_ptr_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(obj_attribs->name_ptr);
  auto path_str = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXLOG_TRACE(kernel,
               "NtQueryFullAttributesFile "
               "path={}",
               path_str);

  Pinned<FileObject> root_file;
  if (obj_attribs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      obj_attribs->root_directory != 0) {
    root_file = REX_KERNEL_OBJECTS()->Get<FileObject>(obj_attribs->root_directory);
    assert_true(static_cast<bool>(root_file));
    assert_always();
  }

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (auto entry = REX_KERNEL_FS()->Stat(target_path)) {
    file_info->creation_time = entry->create_time;
    file_info->last_access_time = entry->access_time;
    file_info->last_write_time = entry->write_time;
    file_info->change_time = entry->write_time;
    file_info->allocation_size = entry->allocation_size;
    file_info->end_of_file = entry->size;
    file_info->attributes = std::to_underlying(entry->attributes);

    REXLOG_TRACE(kernel,
                 "NtQueryFullAttributesFile -> "
                 "0x0 size={}",
                 entry->size);
    return X_STATUS_SUCCESS;
  }

  REXLOG_TRACE(kernel,
               "NtQueryFullAttributesFile -> "
               "X_STATUS_NO_SUCH_FILE");
  return X_STATUS_NO_SUCH_FILE;
}

u32 NtQueryDirectoryFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine,
                               mapped_void apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                               ppc_ptr_t<X_FILE_DIRECTORY_INFORMATION> file_info_ptr, u32 length,
                               ppc_ptr_t<X_ANSI_STRING> file_name, u32 restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  uint32_t info = 0;

  auto file = REX_KERNEL_OBJECTS()->Get<FileObject>(file_handle);
  auto name = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), file_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(name, true)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    // X_FILE_DIRECTORY_INFORMATION dir_info = {0};
    result = FileQueryDirectory(REX_KERNEL_STATE(), *file, file_info_ptr, length, name,
                                restart_scan != 0);
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

u32 NtFlushBuffersFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block_ptr) {
  auto result = X_STATUS_SUCCESS;

  if (io_status_block_ptr) {
    io_status_block_ptr->status = result;
    io_status_block_ptr->information = 0;
  }

  return result;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntopensymboliclinkobject
u32 NtOpenSymbolicLinkObject_entry(mapped_u32 handle_out,
                                   ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  REX_KERNEL_ARG(object_attrs);
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

  if (rex::utf8_starts_with(target_path, "\\??\\")) {
    target_path = target_path.substr(4);  // Strip the full qualifier
  }

  std::optional<std::string> link_path = REX_KERNEL_FS()->FindLink(target_path);
  if (!link_path) {
    return X_STATUS_NO_SUCH_FILE;
  }

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS status = REX_KERNEL_OBJECTS()->Emplace<SymbolicLinkObject>(&handle, AddOptions{},
                                                                      target_path, *link_path);
  if (XFAILED(status)) {
    return status;
  }

  *handle_out = handle;

  return X_STATUS_SUCCESS;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntquerysymboliclinkobject
u32 NtQuerySymbolicLinkObject_entry(u32 handle, ppc_ptr_t<X_ANSI_STRING> target) {
  auto symlink = REX_KERNEL_OBJECTS()->Get<SymbolicLinkObject>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length), symlink->target.size());
  if (length > 0) {
    auto target_buf = REX_KERNEL_MEMORY()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target.c_str(), length);
  }
  target->length = static_cast<uint16_t>(length);
  return X_STATUS_SUCCESS;
}

u32 FscGetCacheElementCount_entry(u32 r3) {
  return 0;
}

u32 FscSetCacheElementCount_entry(u32 unk_0, u32 unk_1) {
  // unk_0 = 0
  // unk_1 looks like a count? in what units? 256 is a common value
  return X_STATUS_SUCCESS;
}

u32 NtDeviceIoControlFile_entry(u32 handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                                u32 io_status_block, u32 io_control_code, mapped_void input_buffer,
                                u32 input_buffer_len, mapped_void output_buffer,
                                u32 output_buffer_len) {
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
    REX_KERNEL_STUB("NtDeviceIoControlFile(0x{:X})", uint32_t(io_control_code));
    assert_always();
    return X_STATUS_INVALID_PARAMETER;
  }

  return X_STATUS_SUCCESS;
}

u32 IoCreateDevice_entry(u32 device_struct, u32 r4, u32 r5, u32 r6, u32 r7, mapped_u32 out_struct) {
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

u32 IoDismountVolumeByFileHandle_entry(u32 handle) {
  REX_KERNEL_STUB("IoDismountVolumeByFileHandle({:#x})", (uint32_t)handle);
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByName_entry(ppc_ptr_t<X_ANSI_STRING> name) {
  REX_KERNEL_STUB("IoDismountVolumeByName");
  return X_STATUS_SUCCESS;
}

u32 IoSynchronousDeviceIoControlRequest_entry(u32 ioctl, mapped_void device_object,
                                              mapped_void input_buffer, u32 input_length,
                                              mapped_void output_buffer, u32 output_length,
                                              mapped_u32 returned_length, u32 unk) {
  REX_KERNEL_STUB("IoSynchronousDeviceIoControlRequest({:#x})", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

u32 StfsCreateDevice_entry(mapped_void device_object, u32 flags, mapped_u32 out_device) {
  REX_KERNEL_STUB("StfsCreateDevice");
  // if (out_device) *out_device = 0;
  return X_STATUS_SUCCESS;
}

u32 StfsControlDevice_entry(mapped_void device_object, u32 ioctl, mapped_void input_buffer,
                            u32 input_length, mapped_void output_buffer, u32 output_length) {
  REX_KERNEL_STUB("StfsControlDevice({:#x})", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__NtCreateFile, rex::kernel::xboxkrnl::NtCreateFile_entry)
REX_EXPORT(__imp__NtOpenFile, rex::kernel::xboxkrnl::NtOpenFile_entry)
REX_EXPORT(__imp__NtReadFile, rex::kernel::xboxkrnl::NtReadFile_entry)
REX_EXPORT(__imp__NtReadFileScatter, rex::kernel::xboxkrnl::NtReadFileScatter_entry)
REX_EXPORT(__imp__NtWriteFile, rex::kernel::xboxkrnl::NtWriteFile_entry)
REX_EXPORT(__imp__NtCreateIoCompletion, rex::kernel::xboxkrnl::NtCreateIoCompletion_entry)
REX_EXPORT(__imp__NtSetIoCompletion, rex::kernel::xboxkrnl::NtSetIoCompletion_entry)
REX_EXPORT(__imp__NtRemoveIoCompletion, rex::kernel::xboxkrnl::NtRemoveIoCompletion_entry)
REX_EXPORT(__imp__NtQueryFullAttributesFile, rex::kernel::xboxkrnl::NtQueryFullAttributesFile_entry)
REX_EXPORT(__imp__NtQueryDirectoryFile, rex::kernel::xboxkrnl::NtQueryDirectoryFile_entry)
REX_EXPORT(__imp__NtFlushBuffersFile, rex::kernel::xboxkrnl::NtFlushBuffersFile_entry)
REX_EXPORT(__imp__NtOpenSymbolicLinkObject, rex::kernel::xboxkrnl::NtOpenSymbolicLinkObject_entry)
REX_EXPORT(__imp__NtQuerySymbolicLinkObject, rex::kernel::xboxkrnl::NtQuerySymbolicLinkObject_entry)
REX_EXPORT(__imp__FscGetCacheElementCount, rex::kernel::xboxkrnl::FscGetCacheElementCount_entry)
REX_EXPORT(__imp__FscSetCacheElementCount, rex::kernel::xboxkrnl::FscSetCacheElementCount_entry)
REX_EXPORT(__imp__NtDeviceIoControlFile, rex::kernel::xboxkrnl::NtDeviceIoControlFile_entry)
REX_EXPORT(__imp__IoCreateDevice, rex::kernel::xboxkrnl::IoCreateDevice_entry)
REX_EXPORT(__imp__IoDismountVolumeByFileHandle,
           rex::kernel::xboxkrnl::IoDismountVolumeByFileHandle_entry)
REX_EXPORT(__imp__IoDismountVolumeByName, rex::kernel::xboxkrnl::IoDismountVolumeByName_entry)
REX_EXPORT(__imp__IoSynchronousDeviceIoControlRequest,
           rex::kernel::xboxkrnl::IoSynchronousDeviceIoControlRequest_entry)
REX_EXPORT(__imp__StfsCreateDevice, rex::kernel::xboxkrnl::StfsCreateDevice_entry)
REX_EXPORT(__imp__StfsControlDevice, rex::kernel::xboxkrnl::StfsControlDevice_entry)

REX_EXPORT_STUB(__imp__IoAcquireDeviceObjectLock);
REX_EXPORT_STUB(__imp__IoAllocateIrp);
REX_EXPORT_STUB(__imp__IoBuildAsynchronousFsdRequest);
REX_EXPORT_STUB(__imp__IoBuildDeviceIoControlRequest);
REX_EXPORT_STUB(__imp__IoBuildSynchronousFsdRequest);
REX_EXPORT_STUB(__imp__IoCallDriver);
REX_EXPORT_STUB(__imp__IoCheckShareAccess);
REX_EXPORT_STUB(__imp__IoCompleteRequest);
REX_EXPORT_STUB(__imp__IoCreateFile);
REX_EXPORT_STUB(__imp__IoDeleteDevice);
REX_EXPORT_STUB(__imp__IoDismountVolume);
REX_EXPORT_STUB(__imp__IoFreeIrp);
REX_EXPORT_STUB(__imp__IoInitializeIrp);
REX_EXPORT_STUB(__imp__IoInvalidDeviceRequest);
REX_EXPORT_STUB(__imp__IoQueueThreadIrp);
REX_EXPORT_STUB(__imp__IoReleaseDeviceObjectLock);
REX_EXPORT_STUB(__imp__IoRemoveShareAccess);
REX_EXPORT_STUB(__imp__IoSetIoCompletion);
REX_EXPORT_STUB(__imp__IoSetShareAccess);
REX_EXPORT_STUB(__imp__IoStartNextPacket);
REX_EXPORT_STUB(__imp__IoStartNextPacketByKey);
REX_EXPORT_STUB(__imp__IoStartPacket);
REX_EXPORT_STUB(__imp__IoSynchronousFsdRequest);
REX_EXPORT_STUB(__imp__NtDeleteFile);
REX_EXPORT_STUB(__imp__NtSetSystemTime);
REX_EXPORT_STUB(__imp__NtWriteFileGather);
REX_EXPORT_STUB(__imp__IoAcquireCancelSpinLock);
REX_EXPORT_STUB(__imp__IoReleaseCancelSpinLock);
REX_EXPORT_STUB(__imp__NtCancelIoFile);
REX_EXPORT_STUB(__imp__NtCancelIoFileEx);
REX_EXPORT_STUB(__imp__SvodCreateDevice);

REX_EXPORT_STUB(__imp__SataCdRomRecordReset);
