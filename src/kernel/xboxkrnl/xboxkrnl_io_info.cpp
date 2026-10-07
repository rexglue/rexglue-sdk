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
#include <rex/runtime/info/file.h>
#include <rex/runtime/info/volume.h>
#include <rex/runtime.h>
#include <rex/runtime/util/string_utils.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xfile.h>
#include <rex/runtime/xiocompletion.h>
#include <rex/runtime/xsymboliclink.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

static bool IsValidPath(const std::string_view s, bool is_pattern) {
  // TODO(gibbed): validate path components individually.
  bool got_asterisk = false;
  for (const auto& c : s) {
    if (c <= 31 || c >= 127) {
      return false;
    }
    if (got_asterisk) {
      if (c != '.') {
        return false;
      }
      got_asterisk = false;
    }
    switch (c) {
      case '"':
      case '+':
      case ',':
      case ';':
      case '<':
      case '=':
      case '>':
      case '|': {
        return false;
      }
      case '*': {
        if (!is_pattern) {
          return false;
        }
        got_asterisk = true;
        break;
      }
      case '?': {
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

uint32_t GetQueryFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileInternalInformation:
      return sizeof(X_FILE_INTERNAL_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileXctdCompressionInformation:
      return sizeof(X_FILE_XCTD_COMPRESSION_INFORMATION);
    case XFileNetworkOpenInformation:
      return sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
    // TODO(gibbed): structures to get the size of.
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

u32 NtQueryInformationFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block_ptr,
                                 mapped_void info_ptr, u32 info_length, u32 info_class) {
  uint32_t minimum_length = GetQueryFileInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  REX_KERNEL_LOOKUP(file, FileObject, file_handle);

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t out_length;

  switch (info_class) {
    case XFileInternalInformation: {
      // Internal unique file pointer. Not sure why anyone would want this.
      // TODO(benvanik): use pointer to fs::entry?
      auto info = info_ptr.as<X_FILE_INTERNAL_INFORMATION*>();
      info->index_number = rex::memory::hash_combine(0, file->file->Info().path);
      out_length = sizeof(*info);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.as<X_FILE_POSITION_INFORMATION*>();
      info->current_byte_offset = FilePosition(*file);
      out_length = sizeof(*info);
      break;
    }
    case XFileSectorInformation: {
      REX_KERNEL_STUB("XFileSectorInformation");
      auto info = info_ptr.as<uint32_t*>();
      size_t fname_hash = rex::memory::hash_combine(82589933LL, file->file->Info().path);
      *info = static_cast<uint32_t>(fname_hash ^ (fname_hash >> 32));
      out_length = sizeof(uint32_t);
      break;
    }
    case XFileXctdCompressionInformation: {
      REX_KERNEL_STUB("NtQueryInformationFile(XFileXctdCompressionInformation)");
      // Files that are XCTD compressed begin with the magic 0x0FF512ED but we
      // shouldn't detect this that way. There's probably a flag somewhere
      // (attributes?) that defines if it's compressed or not.
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    };
    case XFileNetworkOpenInformation: {
      // Make sure we're working with up-to-date information, in case the
      // file size has changed via something other than NtSetInfoFile
      // (eg. seems NtWriteFile might extend the file in some cases)
      file->file->Refresh();

      const filesystem::FileInfo entry = file->file->Info();
      auto info = info_ptr.as<X_FILE_NETWORK_OPEN_INFORMATION*>();
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
      // Requested by XMountUtilityDrive XAM-task
      auto info = info_ptr.as<uint32_t*>();
      *info = 0;  // FILE_BYTE_ALIGNMENT?
      out_length = sizeof(*info);
      break;
    }
    default: {
      // Unsupported.
      REX_ASSERT_ALWAYS();
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    }
  }

  if (io_status_block_ptr) {
    io_status_block_ptr->status = status;
    io_status_block_ptr->information = out_length;
  }

  return status;
}

uint32_t GetSetFileInfoMinimumLength(uint32_t info_class) {
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
    // TODO(gibbed): structures to get the size of.
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

u32 NtSetInformationFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                               mapped_void info_ptr, u32 info_length, u32 info_class) {
  uint32_t minimum_length = GetSetFileInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  REX_KERNEL_LOOKUP(file, FileObject, file_handle);

  X_STATUS result = X_STATUS_SUCCESS;
  uint32_t out_length;

  switch (info_class) {
    case XFileBasicInformation: {
      auto info = info_ptr.as<X_FILE_BASIC_INFORMATION*>();

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
                              static_cast<uint32_t>(info->attributes)))
                          .has_value();
      if (!basic_result) {
        result = X_STATUS_UNSUCCESSFUL;
      }

      out_length = sizeof(*info);
      break;
    }
    case XFileDispositionInformation: {
      auto info = info_ptr.as<X_FILE_DISPOSITION_INFORMATION*>();
      bool delete_on_close = info->delete_file ? true : false;
      file->file->SetDeleteOnClose(delete_on_close);
      out_length = 0;
      REXLOG_WARN(kernel, "NtSetInformationFile set deleting flag for {} on close to: {}",
                  file->file->Info().name, delete_on_close);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.as<X_FILE_POSITION_INFORMATION*>();
      FileSetPosition(*file, info->current_byte_offset);
      out_length = sizeof(*info);
      break;
    }
    case XFileRenameInformation: {
      auto info = info_ptr.as<X_FILE_RENAME_INFORMATION*>();
      auto target_path = util::TranslateAnsiPath(runtime::Current().memory(), &info->ansi_string);
      if (!IsValidPath(target_path, false)) {
        return X_STATUS_OBJECT_NAME_INVALID;
      }

      auto target_file_path = rex::to_path(target_path);
      if (!target_file_path.has_filename()) {
        return X_STATUS_INVALID_PARAMETER;
      }

      Pinned<FileObject> root;
      if (info->root_dir_handle) {
        root = runtime::Current().object_table().Get<FileObject>(info->root_dir_handle);
        if (!root) {
          return X_STATUS_INVALID_HANDLE;
        }
      }
      result = FileRename(*runtime::Current().file_system(), *file, target_path,
                          info->replace_existing != 0, root ? &*root : nullptr);
      out_length = sizeof(*info);
      break;
    }
    case XFileAllocationInformation: {
      auto info = info_ptr.as<X_FILE_ALLOCATION_INFORMATION*>();
      result = FileSetAllocation(*file, info->allocation_size);
      out_length = sizeof(*info);

      file->file->Refresh();
      break;
    }
    case XFileEndOfFileInformation: {
      auto info = info_ptr.as<X_FILE_END_OF_FILE_INFORMATION*>();
      result = FileSetLength(*file, info->end_of_file);
      out_length = sizeof(*info);

      file->file->Refresh();
      break;
    }
    case XFileCompletionInformation: {
      // Info contains IO Completion handle and completion key
      auto info = info_ptr.as<X_FILE_COMPLETION_INFORMATION*>();
      auto handle = static_cast<uint32_t>(info->handle);
      auto key = static_cast<uint32_t>(info->key);
      out_length = sizeof(*info);
      auto port = runtime::Current().object_table().Get<IOCompletionObject>(handle);
      if (!port) {
        result = X_STATUS_INVALID_HANDLE;
      } else {
        FileRegisterIOCompletionPort(runtime::Current(), *file, key, port.handle());
      }
      break;
    }
    default:
      // Unsupported.
      REX_ASSERT_ALWAYS();
      out_length = 0;
      break;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = out_length;
  }

  return result;
}

uint32_t GetQueryVolumeInfoMinimumLength(uint32_t info_class) {
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
                                       ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block_ptr,
                                       mapped_void info_ptr, u32 info_length, u32 info_class) {
  uint32_t minimum_length = GetQueryVolumeInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  REX_KERNEL_LOOKUP(file, FileObject, file_handle);

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t out_length;

  switch (info_class) {
    case XFileFsVolumeInformation: {
      auto info = info_ptr.as<X_FILE_FS_VOLUME_INFORMATION*>();
      info->creation_time = 0;
      info->serial_number = 0;  // set for FATX, but we don't do that
      info->supports_objects = 0;
      info->label_length = 0;
      out_length = offsetof(X_FILE_FS_VOLUME_INFORMATION, label);
      break;
    }
    case XFileFsSizeInformation: {
      const filesystem::VolumeInfo volume = file->file->Volume();
      auto info = info_ptr.as<X_FILE_FS_SIZE_INFORMATION*>();
      info->total_allocation_units = volume.total_allocation_units;
      info->available_allocation_units = volume.available_allocation_units;
      info->sectors_per_allocation_unit = volume.sectors_per_allocation_unit;
      info->bytes_per_sector = volume.bytes_per_sector;
      // TODO(gibbed): sanity check, XCTD userland code seems to require this.
      REX_ASSERT(info->bytes_per_sector == 0x200);
      out_length = sizeof(*info);
      break;
    }
    case XFileFsAttributeInformation: {
      const filesystem::VolumeInfo volume = file->file->Volume();
      const auto& name = volume.name;
      auto info = info_ptr.as<X_FILE_FS_ATTRIBUTE_INFORMATION*>();
      info->attributes = volume.attributes;
      info->component_name_max_length = volume.component_name_max_length;
      info->name_length = static_cast<uint32_t>(name.size());
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
      auto info = info_ptr.as<X_FILE_FS_DEVICE_INFORMATION*>();
      REX_KERNEL_STUB("XFileFsDeviceInformation");
      info->device_type = FILE_DEVICE_UNKNOWN;
      info->characteristics = 0;
      out_length = sizeof(X_FILE_FS_DEVICE_INFORMATION);
      break;
    }
    default: {
      // Unsupported.
      REX_ASSERT_ALWAYS();
      out_length = 0;
      break;
    }
  }

  if (io_status_block_ptr) {
    io_status_block_ptr->status = status;
    io_status_block_ptr->information = out_length;
  }

  return status;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__NtQueryInformationFile, rex::kernel::xboxkrnl::NtQueryInformationFile_entry)
REX_EXPORT(__imp__NtSetInformationFile, rex::kernel::xboxkrnl::NtSetInformationFile_entry)
REX_EXPORT(__imp__NtQueryVolumeInformationFile,
           rex::kernel::xboxkrnl::NtQueryVolumeInformationFile_entry)
