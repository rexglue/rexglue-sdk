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

#include <rex/hash.h>
#include <rex/filesystem/file_stat.h>
#include <rex/logging/log.h>
#include "kernel/categories.h"
#include <rex/memory.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/types.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/io_completion_object.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>
#include "kernel/object_lookup.h"
#include "kernel/path_check.h"
#include "kernel/stub_log.h"
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

u32 GetQueryFileMinimumLength(u32 info_class) {
  switch (info_class) {
    case XFileInternalInformation:
      return sizeof(X_FILE_INTERNAL_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileXctdCompressionInformation:
      return sizeof(X_FILE_XCTD_COMPRESSION_INFORMATION);
    case XFileNetworkOpenInformation:
      return sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
    case XFileModeInformation:
    case XFileAlignmentInformation:
    case XFileSectorInformation:
    case XFileIoPriorityInformation:
      return 4;
    case XFileNameInformation:
    case XFileAllocationInformation:
      return 8;
    case XFileBasicInformation:
      return 40;
    default:
      return 0;
  }
}

u32 NtQueryInformationFile_entry(u32 file_handle, GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                                 MappedVoid info_ptr, u32 info_length, u32 info_class) {
  u32 minimum_length = GetQueryFileMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  REX_KERNEL_LOOKUP(file, FileObject, file_handle);

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  u32 out_length;

  switch (info_class) {
    case XFileInternalInformation: {
      auto info = info_ptr.As<X_FILE_INTERNAL_INFORMATION*>();
      info->index_number = rex::hash_combine(0, file->file->GetInfo().path);
      out_length = sizeof(*info);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.As<X_FILE_POSITION_INFORMATION*>();
      info->current_byte_offset = file->GetPosition();
      out_length = sizeof(*info);
      break;
    }
    case XFileSectorInformation: {
      REX_KERNEL_STUB("XFileSectorInformation");
      auto info = info_ptr.As<u32*>();
      size_t fname_hash = rex::hash_combine(82589933LL, file->file->GetInfo().path);
      *info = static_cast<u32>(fname_hash ^ (fname_hash >> 32));
      out_length = sizeof(u32);
      break;
    }
    case XFileXctdCompressionInformation: {
      REX_KERNEL_STUB("NtQueryInformationFile(XFileXctdCompressionInformation)");
      // XCTD files begin with 0x0FF512ED. Disc layouts tag them in the GDF directory entry too.
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    };
    case XFileNetworkOpenInformation: {
      // The size can change outside NtSetInformationFile, as NtWriteFile can extend the file.
      file->file->Refresh();

      const filesystem::FileStat entry = file->file->GetInfo();
      auto info = info_ptr.As<X_FILE_NETWORK_OPEN_INFORMATION*>();
      info->creation_time = entry.create_time;
      info->last_access_time = entry.access_time;
      info->last_write_time = entry.write_time;
      info->change_time = entry.write_time;
      info->allocation_size = entry.allocation_size;
      info->end_of_file = entry.size;
      info->attributes = std::to_underlying(entry.attributes);
      out_length = sizeof(*info);
      break;
    }
    case XFileAlignmentInformation: {
      auto info = info_ptr.As<u32*>();
      *info = 0;
      out_length = sizeof(*info);
      break;
    }
    default: {
      REXLOG_WARN(kernel, "NtQueryInformationFile class {} is unsupported", info_class);
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    }
  }

  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = out_length;
  }

  return status;
}

u32 GetSetFileMinimumLength(u32 info_class) {
  switch (info_class) {
    case XFileRenameInformation:
      return sizeof(X_FILE_RENAME_INFORMATION);
    case XFileDispositionInformation:
      return sizeof(X_FILE_DISPOSITION_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileCompletionInformation:
      return sizeof(X_FILE_COMPLETION_INFORMATION);
    case XFileAllocationInformation:
      return sizeof(X_FILE_ALLOCATION_INFORMATION);
    case XFileEndOfFileInformation:
      return sizeof(X_FILE_END_OF_FILE_INFORMATION);
    case XFileModeInformation:
    case XFileIoPriorityInformation:
      return 4;
    case XFileMountPartitionInformation:
      return 8;
    case XFileLinkInformation:
      return 16;
    case XFileBasicInformation:
      return 40;
    case XFileMountPartitionsInformation:
      return 152;
    default:
      return 0;
  }
}

u32 NtSetInformationFile_entry(u32 file_handle, GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                               MappedVoid info_ptr, u32 info_length, u32 info_class) {
  u32 minimum_length = GetSetFileMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  REX_KERNEL_LOOKUP(file, FileObject, file_handle);

  X_STATUS result = X_STATUS_SUCCESS;
  u32 out_length;

  switch (info_class) {
    case XFileBasicInformation: {
      auto info = info_ptr.As<X_FILE_BASIC_INFORMATION*>();

      filesystem::FileTimes times;
      if (info->creation_time) {
        times.create = info->creation_time;
      }
      if (info->last_access_time) {
        times.access = info->last_access_time;
      }
      if (info->last_write_time) {
        times.write = info->last_write_time;
      }

      bool basic_result = true;
      if (times.create || times.access || times.write) {
        basic_result &= file->file->SetTimes(times).has_value();
      }
      basic_result &= file->file
                          ->SetAttributes(static_cast<filesystem::FileAttributes>(
                              static_cast<u32>(info->attributes)))
                          .has_value();
      if (!basic_result) {
        result = X_STATUS_UNSUCCESSFUL;
      }

      out_length = sizeof(*info);
      break;
    }
    case XFileDispositionInformation: {
      auto info = info_ptr.As<X_FILE_DISPOSITION_INFORMATION*>();
      bool delete_on_close = info->delete_file ? true : false;
      file->file->SetDeleteOnClose(delete_on_close);
      out_length = 0;
      REXLOG_WARN(kernel, "NtSetInformationFile set the delete on close flag of {} to {}",
                  file->file->GetInfo().name, delete_on_close);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.As<X_FILE_POSITION_INFORMATION*>();
      file->SetPosition(info->current_byte_offset);
      out_length = sizeof(*info);
      break;
    }
    case XFileRenameInformation: {
      auto info = info_ptr.As<X_FILE_RENAME_INFORMATION*>();
      auto target_path =
          kernel::TranslateAnsiPath(kernel::GetCurrentState().GetMemory(), &info->ansi_string);
      if (!IsValidPath(target_path, PathUse::kNewName)) {
        return X_STATUS_OBJECT_NAME_INVALID;
      }

      auto target_file_path = rex::string::to_path(target_path);
      if (!target_file_path.has_filename()) {
        return X_STATUS_INVALID_PARAMETER;
      }

      Pinned<FileObject> root;
      if (info->root_dir_handle) {
        root = kernel::GetCurrent().GetObjectTable().Get<FileObject>(info->root_dir_handle);
        if (!root) {
          return X_STATUS_INVALID_HANDLE;
        }
      }
      result = file->Rename(*kernel::GetCurrentState().GetFileSystem(), target_path,
                            info->replace_existing != 0, root ? &*root : nullptr);
      out_length = sizeof(*info);
      break;
    }
    case XFileAllocationInformation: {
      auto info = info_ptr.As<X_FILE_ALLOCATION_INFORMATION*>();
      result = file->SetAllocation(info->allocation_size);
      out_length = sizeof(*info);

      file->file->Refresh();
      break;
    }
    case XFileEndOfFileInformation: {
      auto info = info_ptr.As<X_FILE_END_OF_FILE_INFORMATION*>();
      result = file->SetLength(info->end_of_file);
      out_length = sizeof(*info);

      file->file->Refresh();
      break;
    }
    case XFileCompletionInformation: {
      auto info = info_ptr.As<X_FILE_COMPLETION_INFORMATION*>();
      auto handle = static_cast<u32>(info->handle);
      auto key = static_cast<u32>(info->key);
      out_length = sizeof(*info);
      auto port = kernel::GetCurrent().GetObjectTable().Get<IOCompletionObject>(handle);
      if (!port) {
        result = X_STATUS_INVALID_HANDLE;
      } else {
        file->RegisterIOCompletionPort(kernel::GetCurrent(), key, port.GetHandle());
      }
      break;
    }
    default:
      REXLOG_WARN(kernel, "NtSetInformationFile class {} is unsupported", info_class);
      out_length = 0;
      break;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = out_length;
  }

  return result;
}

u32 GetQueryVolumeMinimumLength(u32 info_class) {
  switch (info_class) {
    case XFileFsVolumeInformation:
      return sizeof(X_FILE_FS_VOLUME_INFORMATION);
    case XFileFsSizeInformation:
      return sizeof(X_FILE_FS_SIZE_INFORMATION);
    case XFileFsDeviceInformation:
      return sizeof(X_FILE_FS_DEVICE_INFORMATION);
    case XFileFsAttributeInformation:
      return sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION);
    default:
      REX_KERNEL_STUB("Info Class 0x{:08x}", info_class);
      return 0;
  }
}

u32 NtQueryVolumeInformationFile_entry(u32 file_handle,
                                       GuestPtrT<X_IO_STATUS_BLOCK> io_status_block,
                                       MappedVoid info_ptr, u32 info_length, u32 info_class) {
  u32 minimum_length = GetQueryVolumeMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  REX_KERNEL_LOOKUP(file, FileObject, file_handle);

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  u32 out_length;

  switch (info_class) {
    case XFileFsVolumeInformation: {
      auto info = info_ptr.As<X_FILE_FS_VOLUME_INFORMATION*>();
      info->creation_time = 0;
      info->serial_number = 0;
      info->supports_objects = 0;
      info->label_length = 0;
      out_length = offsetof(X_FILE_FS_VOLUME_INFORMATION, label);
      break;
    }
    case XFileFsSizeInformation: {
      const filesystem::VolumeStat volume = file->file->GetVolume();
      auto info = info_ptr.As<X_FILE_FS_SIZE_INFORMATION*>();
      info->total_allocation_units = volume.total_allocation_units;
      info->available_allocation_units = volume.available_allocation_units;
      info->sectors_per_allocation_unit = volume.sectors_per_allocation_unit;
      info->bytes_per_sector = volume.bytes_per_sector;
      REX_ASSERT(info->bytes_per_sector == 0x200);
      out_length = sizeof(*info);
      break;
    }
    case XFileFsAttributeInformation: {
      const filesystem::VolumeStat volume = file->file->GetVolume();
      const auto& name = volume.name;
      auto info = info_ptr.As<X_FILE_FS_ATTRIBUTE_INFORMATION*>();
      info->attributes = volume.attributes;
      info->component_name_max_length = static_cast<i32>(volume.component_name_max_length);
      info->name_length = static_cast<u32>(name.size());
      if (info_length >= 12 + name.size()) {
        std::memcpy(info->name, name.data(), name.size());
        out_length = offsetof(X_FILE_FS_ATTRIBUTE_INFORMATION, name) + info->name_length;
      } else {
        status = X_STATUS_BUFFER_OVERFLOW;
        out_length = offsetof(X_FILE_FS_ATTRIBUTE_INFORMATION, name);
      }
      break;
    }
    case XFileFsDeviceInformation: {
      auto info = info_ptr.As<X_FILE_FS_DEVICE_INFORMATION*>();
      REX_KERNEL_STUB("XFileFsDeviceInformation");
      info->device_type = kFileDeviceUnknown;
      info->characteristics = 0;
      out_length = sizeof(X_FILE_FS_DEVICE_INFORMATION);
      break;
    }
    default: {
      out_length = 0;
      break;
    }
  }

  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = out_length;
  }

  return status;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__NtQueryInformationFile, rex::kernel::xboxkrnl::NtQueryInformationFile_entry)
REX_EXPORT(__imp__NtSetInformationFile, rex::kernel::xboxkrnl::NtSetInformationFile_entry)
REX_EXPORT(__imp__NtQueryVolumeInformationFile,
           rex::kernel::xboxkrnl::NtQueryVolumeInformationFile_entry)
