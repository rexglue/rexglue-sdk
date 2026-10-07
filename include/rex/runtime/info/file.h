#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/runtime/xio.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

// https://github.com/oukiar/vdash/blob/master/vdash/include/kernel.h
enum X_FILE_INFORMATION_CLASS {
  XFileDirectoryInformation = 1,
  XFileFullDirectoryInformation,
  XFileBothDirectoryInformation,
  XFileBasicInformation,
  XFileStandardInformation,
  XFileInternalInformation,
  XFileEaInformation,
  XFileAccessInformation,
  XFileNameInformation,
  XFileRenameInformation,
  XFileLinkInformation,
  XFileNamesInformation,
  XFileDispositionInformation,
  XFilePositionInformation,
  XFileFullEaInformation,
  XFileModeInformation,
  XFileAlignmentInformation,
  XFileAllInformation,
  XFileAllocationInformation,
  XFileEndOfFileInformation,
  XFileAlternateNameInformation,
  XFileStreamInformation,
  XFileMountPartitionInformation,
  XFileMountPartitionsInformation,
  XFilePipeRemoteInformation,
  XFileSectorInformation,
  XFileXctdCompressionInformation,
  XFileCompressionInformation,
  XFileObjectIdInformation,
  XFileCompletionInformation,
  XFileMoveClusterInformation,
  XFileIoPriorityInformation,
  XFileReparsePointInformation,
  XFileNetworkOpenInformation,
  XFileAttributeTagInformation,
  XFileTrackingInformation,
  XFileMaximumInformation
};

#pragma pack(push, 1)

// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_basic_information
struct X_FILE_BASIC_INFORMATION {
  be_u64 creation_time;
  be_u64 last_access_time;
  be_u64 last_write_time;
  be_u64 change_time;
  be_u64 attributes;
};
REX_STATIC_ASSERT_SIZE(X_FILE_BASIC_INFORMATION, 0x28);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_internal_information
struct X_FILE_INTERNAL_INFORMATION {
  be_u64 index_number;
};
REX_STATIC_ASSERT_SIZE(X_FILE_INTERNAL_INFORMATION, 8);

// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
struct X_FILE_RENAME_INFORMATION {
  be_u32 replace_existing;
  be_u32 root_dir_handle;
  X_ANSI_STRING ansi_string;
};
REX_STATIC_ASSERT_SIZE(X_FILE_RENAME_INFORMATION, 16);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_disposition_information
struct X_FILE_DISPOSITION_INFORMATION {
  uint8_t delete_file;
};
REX_STATIC_ASSERT_SIZE(X_FILE_DISPOSITION_INFORMATION, 1);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_position_information
struct X_FILE_POSITION_INFORMATION {
  be_u64 current_byte_offset;
};
REX_STATIC_ASSERT_SIZE(X_FILE_POSITION_INFORMATION, 8);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_end_of_file_information
struct X_FILE_END_OF_FILE_INFORMATION {
  be_u64 end_of_file;
};
REX_STATIC_ASSERT_SIZE(X_FILE_END_OF_FILE_INFORMATION, 8);

// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_allocation_information
struct X_FILE_ALLOCATION_INFORMATION {
  be_u64 allocation_size;
};
REX_STATIC_ASSERT_SIZE(X_FILE_ALLOCATION_INFORMATION, 8);

struct X_FILE_XCTD_COMPRESSION_INFORMATION {
  be_u32 unknown;
};
REX_STATIC_ASSERT_SIZE(X_FILE_XCTD_COMPRESSION_INFORMATION, 4);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_completion_information
struct X_FILE_COMPLETION_INFORMATION {
  be_u32 handle;
  be_u32 key;
};
REX_STATIC_ASSERT_SIZE(X_FILE_COMPLETION_INFORMATION, 8);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_network_open_information
struct X_FILE_NETWORK_OPEN_INFORMATION {
  be_u64 creation_time;
  be_u64 last_access_time;
  be_u64 last_write_time;
  be_u64 change_time;
  be_u64 allocation_size;
  be_u64 end_of_file;  // size in bytes
  be_u32 attributes;
  be_u32 pad;
};
REX_STATIC_ASSERT_SIZE(X_FILE_NETWORK_OPEN_INFORMATION, 56);

#pragma pack(pop)

}  // namespace runtime
}  // namespace rex
