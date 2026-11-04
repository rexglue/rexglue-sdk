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

#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_open.h>
#include <rex/filesystem/file.h>
#include <rex/logging/log.h>
#include "kernel/categories.h"
#include <rex/memory.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/types.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/io_completion_object.h>
#include <rex/kernel/object/symbolic_link_object.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include "kernel/argument_check.h"
#include "kernel/object_lookup.h"
#include "kernel/path_check.h"
#include "kernel/stub_log.h"
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

struct CreateOptions {
  static const u32 kFileDirectoryFile = 0x00000001;
  static const u32 kFileSequentialOnly = 0x00000004;
  static const u32 kFileSynchronousIOAlert = 0x00000010;
  static const u32 kFileSynchronousIONonAlert = 0x00000020;
  static const u32 kFileNonDirectoryFile = 0x00000040;
  static const u32 kFileRandomAccess = 0x00000800;
};

static bool QueueCompletionAPC(u32 apc_routine, u32 apc_context, u32 io_status_block,
                               X_STATUS result) {
  const u32 routine = apc_routine & ~3u;
  if (!routine || (result != X_STATUS_SUCCESS && result != X_STATUS_PENDING)) {
    return false;
  }
  KernelThread::GetCallingThread()->EnqueueAPC(routine, apc_context, io_status_block, 0);
  return true;
}

u32 NtCreateFile_entry(MappedU32 handle_out, u32 desired_access,
                       GuestPtrT<X_OBJECT_ATTRIBUTES> object_attrs,
                       GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                       [[maybe_unused]] MappedU64 allocation_size_ptr, u32 file_attributes,
                       u32 share_access, u32 creation_disposition, u32 create_options) {
  if (!object_attrs) {
    // object_attrs is not optional, though some titles pass null.
    return X_STATUS_INVALID_PARAMETER;
  }
  if (creation_disposition > std::to_underlying(filesystem::Disposition::kOverwriteIf)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_ASSERT(handle_out != nullptr);

  auto object_name = kernel::GetCurrentState().GetMemory()->TranslateVirtual<X_ANSI_STRING*>(
      object_attrs->name_ptr);

  auto target_path = StripDosDevicePrefix(
      kernel::TranslateAnsiPath(kernel::GetCurrentState().GetMemory(), object_name));
  REXKRNL_TRACE(
      "NtCreateFile "
      "path={} access={:#x} attrs={:#x} share={:#x} disp={:#x} options={:#x}",
      target_path, static_cast<u32>(desired_access), static_cast<u32>(file_attributes),
      static_cast<u32>(share_access), static_cast<u32>(creation_disposition),
      static_cast<u32>(create_options));

  if (!IsValidPath(target_path, PathUse::kExisting)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  Pinned<FileObject> root_file;
  // 0xFFFFFFFD is ObDosDevices.
  if (object_attrs->root_directory != 0xFFFFFFFD && object_attrs->root_directory != 0) {
    root_file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(object_attrs->root_directory);
    if (!root_file) {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  filesystem::OpenOptions options;
  options.disposition = static_cast<filesystem::Disposition>(creation_disposition);
  options.access = static_cast<filesystem::Access>(desired_access);
  options.create_directory = (create_options & CreateOptions::kFileDirectoryFile) != 0;
  options.require_file = (create_options & CreateOptions::kFileNonDirectoryFile) != 0;
  auto opened = root_file ? kernel::GetCurrentState().GetFileSystem()->OpenRelative(
                                *root_file->file, target_path, options)
                          : kernel::GetCurrentState().GetFileSystem()->Open(target_path, options);
  X_STATUS result = X_STATUS_SUCCESS;
  filesystem::OpenAction file_action = filesystem::OpenAction::kDoesNotExist;
  if (opened) {
    file_action = opened->action;
  } else {
    result = ToXStatus(opened.error());
    if (filesystem::CodeOf(opened.error()) == filesystem::FileSystemError::kExists) {
      file_action = filesystem::OpenAction::kExists;
    }
  }
  X_HANDLE handle = kXInvalidHandleValue;
  if (XSUCCEEDED(result)) {
    // desired_access must carry the SYNCHRONIZE flag when synchronous.
    bool synchronous = (create_options & CreateOptions::kFileSynchronousIOAlert) ||
                       (create_options & CreateOptions::kFileSynchronousIONonAlert);
    auto added = kernel::GetCurrent().GetObjectTable().Emplace<FileObject>(
        AddOptions{}, std::move(opened->file), synchronous);
    if (added) {
      handle = *added;
    }
    result = ToXStatus(added);
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = static_cast<u32>(file_action);
  }

  *handle_out = handle;
  if (XFAILED(result)) {
    REXKRNL_WARN(
        "NtCreateFile failed "
        "path='{}' -> {:#x}",
        target_path, result);
  } else {
    REXKRNL_TRACE(
        "NtCreateFile -> "
        "{:#x} handle={:#x}",
        result, handle);
  }
  return result;
}

u32 NtOpenFile_entry(MappedU32 handle_out, u32 desired_access,
                     GuestPtrT<X_OBJECT_ATTRIBUTES> object_attrs,
                     GuestPtrT<X_IO_STATUS_BLOCK> io_status_block, u32 open_options) {
  return NtCreateFile_entry(handle_out, desired_access, object_attrs, io_status_block, nullptr, 0,
                            0, std::to_underlying(filesystem::Disposition::kOpen), open_options);
}

/**
 * Reads from a file, at the current position when byte_offset_ptr is null. Bit 0 of the APC
 * routine is masked off.
 *
 * @param event_handle The event to signal on completion, or 0.
 * @param apc_routine_ptr The APC routine to queue on completion.
 * @param io_status_block Receives the status and the bytes read.
 * @param buffer Receives the data.
 * @param buffer_length The buffer size in bytes.
 * @param byte_offset_ptr The file offset, or null.
 * @return The status.
 */
u32 NtReadFile_entry(u32 file_handle, u32 event_handle, MappedVoid apc_routine_ptr,
                     MappedVoid apc_context, GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                     MappedVoid buffer, u32 buffer_length, MappedU64 byte_offset_ptr) {
  u64 byte_offset = byte_offset_ptr ? static_cast<u64>(*byte_offset_ptr) : 0;
  const bool apc_requested = (static_cast<u32>(apc_routine_ptr) & ~1u) != 0;
  REXKRNL_TRACE(
      "NtReadFile "
      "handle={:#x} event={:#x} apc={:#x} apc_ctx={:#x} iosb={:#x} buf={:#x} len={:#x} offset={}",
      static_cast<u32>(file_handle), static_cast<u32>(event_handle),
      apc_routine_ptr.GetGuestAddress(), apc_context.GetGuestAddress(),
      io_status_block.GetGuestAddress(), buffer.GetGuestAddress(), static_cast<u32>(buffer_length),
      byte_offset_ptr ? static_cast<i64>(byte_offset) : -1);
  X_STATUS result = X_STATUS_SUCCESS;
  bool apc_queued = false;

  bool signal_event = false;
  auto ev = kernel::GetCurrent().GetObjectTable().Get<EventObject>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous) {
      u32 bytes_read = 0;
      result = file->Read(kernel::GetCurrent(), buffer.GetGuestAddress(), buffer_length,
                          byte_offset_ptr ? static_cast<u64>(*byte_offset_ptr) : -1, &bytes_read,
                          apc_context.GetGuestAddress());
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      apc_queued =
          QueueCompletionAPC(static_cast<u32>(apc_routine_ptr), apc_context.GetGuestAddress(),
                             io_status_block.GetGuestAddress(), result);

      if (!file->is_synchronous && result != X_STATUS_END_OF_FILE) {
        result = X_STATUS_PENDING;
      }

      // Signal the event only after the info is written.
      signal_event = true;
    } else {
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
    ev->Set();
  }

  if (file) {
    REXKRNL_TRACE(
        "NtReadFile -> "
        "{:#x} (sync={}, iosb_status={:#x}, iosb_info={}, ev_signaled={}, apc_requested={}, "
        "apc_queued={})",
        result, file->is_synchronous,
        io_status_block ? static_cast<u32>(io_status_block->status) : 0xDEAD,
        io_status_block ? static_cast<u32>(io_status_block->information) : 0, ev && signal_event,
        apc_requested, apc_queued);
  } else {
    REXKRNL_TRACE(
        "NtReadFile -> "
        "{:#x} (apc_requested={}, apc_queued={})",
        result, apc_requested, apc_queued);
  }
  return result;
}

u32 NtReadFileScatter_entry(u32 file_handle, u32 event_handle, MappedVoid apc_routine_ptr,
                            MappedVoid apc_context, GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                            MappedU32 segment_array, u32 length, MappedU64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = kernel::GetCurrent().GetObjectTable().Get<EventObject>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous) {
      u32 bytes_read = 0;
      result = file->ReadScatter(kernel::GetCurrent(), segment_array.GetGuestAddress(), length,
                                 byte_offset_ptr ? static_cast<u64>(*byte_offset_ptr) : -1,
                                 &bytes_read, apc_context.GetGuestAddress());
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      QueueCompletionAPC(static_cast<u32>(apc_routine_ptr), apc_context.GetGuestAddress(),
                         io_status_block.GetGuestAddress(), result);

      if (!file->is_synchronous) {
        result = X_STATUS_PENDING;
      }

      // Signal the event only after the info is written.
      signal_event = true;
    } else {
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
    ev->Set();
  }

  return result;
}

u32 NtWriteFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, MappedVoid apc_context,
                      GuestPtrT<X_IO_STATUS_BLOCK> io_status_block, MappedVoid buffer,
                      u32 buffer_length, MappedU64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = kernel::GetCurrent().GetObjectTable().Get<EventObject>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous) {
      u32 bytes_written = 0;
      result = file->Write(kernel::GetCurrent(), buffer.GetGuestAddress(), buffer_length,
                           byte_offset_ptr ? static_cast<u64>(*byte_offset_ptr) : -1,
                           &bytes_written, apc_context.GetGuestAddress());

      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_written;
      }

      QueueCompletionAPC(apc_routine, apc_context.GetGuestAddress(),
                         io_status_block.GetGuestAddress(), result);

      if (!file->is_synchronous) {
        result = X_STATUS_PENDING;
      }

      // Signal the event only after the info is written.
      signal_event = true;
    } else {
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
    ev->Set();
  }

  return result;
}

u32 NtCreateIoCompletion_entry(MappedU32 handle_out, [[maybe_unused]] u32 desired_access,
                               [[maybe_unused]] MappedVoid object_attrs,
                               [[maybe_unused]] u32 num_concurrent_threads) {
  X_HANDLE handle = 0;
  auto added = kernel::GetCurrent().GetObjectTable().Emplace<IOCompletionObject>(AddOptions{});
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XSUCCEEDED(status) && handle_out) {
    *handle_out = handle;
  }

  return status;
}

u32 NtSetIoCompletion_entry(u32 handle, u32 key_context, u32 apc_context, u32 completion_status,
                            u32 num_bytes) {
  REX_KERNEL_LOOKUP(port, IOCompletionObject, handle);

  IONotification notification;
  notification.key = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  port->Queue(notification);
  return X_STATUS_SUCCESS;
}

u32 NtRemoveIoCompletion_entry(u32 handle, MappedU32 key_context, MappedU32 apc_context,
                               GuestPtrT<X_IO_STATUS_BLOCK> io_status_block, MappedU64 timeout) {
  X_STATUS status = X_STATUS_SUCCESS;

  REX_KERNEL_LOOKUP(port, IOCompletionObject, handle);

  u64 timeout_ticks =
      timeout ? static_cast<u32>(*timeout) : static_cast<u64>(std::numeric_limits<i64>::min());
  IONotification notification;
  if (port->Wait(timeout_ticks, &notification)) {
    if (key_context) {
      *key_context = notification.key;
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

static Result<filesystem::FileStat> StatRelative(filesystem::IFile& root, std::string_view path) {
  REX_TRY(filesystem::Opened opened, kernel::GetCurrentState().GetFileSystem()->OpenRelative(
                                         root, path, filesystem::OpenOptions{}));
  return opened.file->GetInfo();
}

u32 NtQueryFullAttributesFile_entry(GuestPtrT<X_OBJECT_ATTRIBUTES> object_attrs,
                                    GuestPtrT<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name = kernel::GetCurrentState().GetMemory()->TranslateVirtual<X_ANSI_STRING*>(
      object_attrs->name_ptr);
  auto path_str = kernel::TranslateAnsiPath(kernel::GetCurrentState().GetMemory(), object_name);
  REXKRNL_TRACE(
      "NtQueryFullAttributesFile "
      "path={}",
      path_str);

  Pinned<FileObject> root_file;
  // 0xFFFFFFFD is ObDosDevices.
  if (object_attrs->root_directory != 0xFFFFFFFD && object_attrs->root_directory != 0) {
    root_file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(object_attrs->root_directory);
    if (!root_file) {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto target_path = StripDosDevicePrefix(
      kernel::TranslateAnsiPath(kernel::GetCurrentState().GetMemory(), object_name));

  if (!IsValidPath(target_path, PathUse::kExisting)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (auto entry = root_file ? StatRelative(*root_file->file, target_path)
                             : kernel::GetCurrentState().GetFileSystem()->Stat(target_path)) {
    file_info->creation_time = entry->create_time;
    file_info->last_access_time = entry->access_time;
    file_info->last_write_time = entry->write_time;
    file_info->change_time = entry->write_time;
    file_info->allocation_size = entry->allocation_size;
    file_info->end_of_file = entry->size;
    file_info->attributes = std::to_underlying(entry->attributes);

    REXKRNL_TRACE(
        "NtQueryFullAttributesFile -> "
        "0x0 size={}",
        entry->size);
    return X_STATUS_SUCCESS;
  }

  REXKRNL_TRACE(
      "NtQueryFullAttributesFile -> "
      "X_STATUS_NO_SUCH_FILE");
  return X_STATUS_NO_SUCH_FILE;
}

u32 NtQueryDirectoryFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine,
                               MappedVoid apc_context, GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                               GuestPtrT<X_FILE_DIRECTORY_INFORMATION> file_info_ptr, u32 length,
                               GuestPtrT<X_ANSI_STRING> file_name, u32 restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  u32 info = 0;

  auto file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(file_handle);
  auto name = kernel::TranslateAnsiPath(kernel::GetCurrentState().GetMemory(), file_name);

  if (!IsValidPath(name, PathUse::kPattern)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    result =
        file->QueryDirectory(kernel::GetCurrent(), file_info_ptr, length, name, restart_scan != 0);
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

  if (XSUCCEEDED(result)) {
    if (auto ev = kernel::GetCurrent().GetObjectTable().Get<EventObject>(event_handle)) {
      ev->Set();
    }
    QueueCompletionAPC(apc_routine, apc_context.GetGuestAddress(),
                       io_status_block.GetGuestAddress(), result);
  }

  return result;
}

u32 NtFlushBuffersFile_entry(u32 file_handle, GuestPtrT<X_IO_STATUS_BLOCK> io_status_block) {
  X_STATUS result = X_STATUS_INVALID_HANDLE;
  if (auto file = kernel::GetCurrent().GetObjectTable().Get<FileObject>(file_handle)) {
    auto flushed = file->file->Flush();
    result = flushed ? X_STATUS_SUCCESS : ToXStatus(flushed.error());
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  return result;
}

u32 NtOpenSymbolicLinkObject_entry(MappedU32 handle_out,
                                   GuestPtrT<X_OBJECT_ATTRIBUTES> object_attrs) {
  REX_KERNEL_REQUIRE_NONNULL(object_attrs);
  REX_ASSERT(handle_out != nullptr);

  auto object_name = kernel::GetCurrentState().GetMemory()->TranslateVirtual<X_ANSI_STRING*>(
      object_attrs->name_ptr);

  auto target_path = StripDosDevicePrefix(
      kernel::TranslateAnsiPath(kernel::GetCurrentState().GetMemory(), object_name));

  if (!IsValidPath(target_path, PathUse::kExisting)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  std::optional<std::string> link_path =
      kernel::GetCurrentState().GetFileSystem()->FindLink(target_path);
  if (!link_path) {
    return X_STATUS_NO_SUCH_FILE;
  }

  X_HANDLE handle = kXInvalidHandleValue;
  auto added = kernel::GetCurrent().GetObjectTable().Emplace<SymbolicLinkObject>(
      AddOptions{}, target_path, *link_path);
  if (added) {
    handle = *added;
  }
  X_STATUS status = ToXStatus(added);
  if (XFAILED(status)) {
    return status;
  }

  *handle_out = handle;

  return X_STATUS_SUCCESS;
}

u32 NtQuerySymbolicLinkObject_entry(u32 handle, GuestPtrT<X_ANSI_STRING> target) {
  auto symlink = kernel::GetCurrent().GetObjectTable().Get<SymbolicLinkObject>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length), symlink->target.size());
  if (length > 0) {
    auto target_buf = kernel::GetCurrentState().GetMemory()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target.c_str(), length);
  }
  target->length = static_cast<u16>(length);
  return X_STATUS_SUCCESS;
}

u32 FscGetCacheElementCount_entry([[maybe_unused]] u32 r3) {
  return 0;
}

/**
 * Ignores the element count, which is commonly 256.
 *
 * @param unk_0 Unused.
 * @param unk_1 Unused.
 * @return X_STATUS_SUCCESS.
 */
u32 FscSetCacheElementCount_entry([[maybe_unused]] u32 unk_0, [[maybe_unused]] u32 unk_1) {
  return X_STATUS_SUCCESS;
}

struct X_DISK_CACHE_GEOMETRY {
  be_u32 sector_count;
  be_u32 sector_size;
};
REX_STATIC_ASSERT_SIZE(X_DISK_CACHE_GEOMETRY, 8);

struct X_DISK_CACHE_PARTITION {
  be_u64 start_offset;
  be_u64 length;
};
REX_STATIC_ASSERT_SIZE(X_DISK_CACHE_PARTITION, 0x10);

/**
 * Answers only the disk geometry and partition requests of XMountUtilityDrive cache mounting, which
 * validates the returned values.
 *
 * @param handle The file handle.
 * @param event_handle The event to signal on completion, or 0.
 * @param apc_routine The APC routine to queue on completion.
 * @param io_status_block Guest address of the status block, or 0.
 * @param input_buffer Unused.
 * @param input_buffer_len Unused.
 * @param output_buffer Receives the geometry or partition data.
 * @param output_buffer_len The output buffer size in bytes.
 * @return The status.
 */
u32 NtDeviceIoControlFile_entry(u32 handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                                u32 io_status_block, u32 io_control_code,
                                [[maybe_unused]] MappedVoid input_buffer,
                                [[maybe_unused]] u32 input_buffer_len, MappedVoid output_buffer,
                                u32 output_buffer_len) {
  const auto file = kernel::GetCurrent().GetObjectTable().Get(handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }
  if (file.GetType() != ObjectType::kFile) {
    return X_STATUS_OBJECT_TYPE_MISMATCH;
  }

  const u32 cache_size = 0xFF000;

  constexpr u32 kXIOCTLDiskGetDriveGeometry = 0x70000;
  constexpr u32 kXIOCTLDiskGetPartitionInfo = 0x74004;

  u32 information = 0;
  if (io_control_code == kXIOCTLDiskGetDriveGeometry) {
    if (output_buffer_len < 0x8) {
      REX_ASSERT_ALWAYS();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    auto* geometry = output_buffer.As<X_DISK_CACHE_GEOMETRY*>();
    geometry->sector_count = cache_size / 512;
    geometry->sector_size = 512;
    information = sizeof(X_DISK_CACHE_GEOMETRY);
  } else if (io_control_code == kXIOCTLDiskGetPartitionInfo) {
    if (output_buffer_len < 0x10) {
      REX_ASSERT_ALWAYS();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    auto* partition = output_buffer.As<X_DISK_CACHE_PARTITION*>();
    partition->start_offset = 0;
    partition->length = cache_size;
    information = sizeof(X_DISK_CACHE_PARTITION);
  } else {
    REX_KERNEL_STUB("NtDeviceIoControlFile(0x{:X})", static_cast<u32>(io_control_code));
    REX_ASSERT_ALWAYS();
    return X_STATUS_INVALID_PARAMETER;
  }

  if (io_status_block) {
    auto* status_block =
        kernel::GetCurrentState().GetMemory()->TranslateVirtual<X_IO_STATUS_BLOCK*>(
            io_status_block);
    status_block->status = X_STATUS_SUCCESS;
    status_block->information = information;
  }
  if (auto ev = kernel::GetCurrent().GetObjectTable().Get<EventObject>(event_handle)) {
    ev->Set();
  }
  QueueCompletionAPC(apc_routine, apc_context, io_status_block, X_STATUS_SUCCESS);
  return X_STATUS_SUCCESS;
}

/**
 * Allocates a device structure for the XMountUtilityDrive XAM task, with a 0x1000 byte buffer
 * pointed to at offset 0x18.
 *
 * @param device_struct Unused.
 * @param r4 Unused.
 * @param r5 Unused.
 * @param r6 Unused.
 * @param r7 Unused.
 * @param out_struct Receives the guest address of the device structure.
 * @return X_STATUS_SUCCESS.
 */
u32 IoCreateDevice_entry([[maybe_unused]] u32 device_struct, [[maybe_unused]] u32 r4,
                         [[maybe_unused]] u32 r5, [[maybe_unused]] u32 r6, [[maybe_unused]] u32 r7,
                         MappedU32 out_struct) {
  auto out_guest = kernel::GetCurrentState().GetMemory()->SystemHeapAlloc(0x24);

  auto out = kernel::GetCurrentState().GetMemory()->TranslateVirtual<u8*>(out_guest);
  memset(out, 0, 0x24);

  auto out_guest2 = kernel::GetCurrentState().GetMemory()->SystemHeapAlloc(0x1000);
  memory::store<be_u32>(out + 0x18, out_guest2);

  *out_struct = out_guest;
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByFileHandle_entry(u32 handle) {
  REX_KERNEL_STUB("IoDismountVolumeByFileHandle({:#x})", static_cast<u32>(handle));
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByName_entry([[maybe_unused]] GuestPtrT<X_ANSI_STRING> name) {
  REX_KERNEL_STUB("IoDismountVolumeByName");
  return X_STATUS_SUCCESS;
}

u32 IoSynchronousDeviceIoControlRequest_entry(u32 ioctl, [[maybe_unused]] MappedVoid device_object,
                                              [[maybe_unused]] MappedVoid input_buffer,
                                              [[maybe_unused]] u32 input_length,
                                              [[maybe_unused]] MappedVoid output_buffer,
                                              [[maybe_unused]] u32 output_length,
                                              [[maybe_unused]] MappedU32 returned_length,
                                              [[maybe_unused]] u32 unk) {
  REX_KERNEL_STUB("IoSynchronousDeviceIoControlRequest({:#x})", static_cast<u32>(ioctl));
  return X_STATUS_SUCCESS;
}

u32 StfsCreateDevice_entry([[maybe_unused]] MappedVoid device_object, [[maybe_unused]] u32 flags,
                           [[maybe_unused]] MappedU32 out_device) {
  REX_KERNEL_STUB("StfsCreateDevice");
  return X_STATUS_SUCCESS;
}

u32 StfsControlDevice_entry([[maybe_unused]] MappedVoid device_object, u32 ioctl,
                            [[maybe_unused]] MappedVoid input_buffer,
                            [[maybe_unused]] u32 input_length,
                            [[maybe_unused]] MappedVoid output_buffer,
                            [[maybe_unused]] u32 output_length) {
  REX_KERNEL_STUB("StfsControlDevice({:#x})", static_cast<u32>(ioctl));
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
