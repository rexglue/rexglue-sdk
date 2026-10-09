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

#pragma once

#include <rex/runtime/xio.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

/** File information class selecting the layout of a query or set request. */
enum X_FILE_INFORMATION_CLASS {
  XFileDirectoryInformation = 1,   /**< File directory information class. */
  XFileFullDirectoryInformation,   /**< File full directory information class. */
  XFileBothDirectoryInformation,   /**< File both directory information class. */
  XFileBasicInformation,           /**< File basic information class. */
  XFileStandardInformation,        /**< File standard information class. */
  XFileInternalInformation,        /**< File internal information class. */
  XFileEaInformation,              /**< File EA information class. */
  XFileAccessInformation,          /**< File access information class. */
  XFileNameInformation,            /**< File name information class. */
  XFileRenameInformation,          /**< File rename information class. */
  XFileLinkInformation,            /**< File link information class. */
  XFileNamesInformation,           /**< File names information class. */
  XFileDispositionInformation,     /**< File disposition information class. */
  XFilePositionInformation,        /**< File position information class. */
  XFileFullEaInformation,          /**< File full EA information class. */
  XFileModeInformation,            /**< File mode information class. */
  XFileAlignmentInformation,       /**< File alignment information class. */
  XFileAllInformation,             /**< File all information class. */
  XFileAllocationInformation,      /**< File allocation information class. */
  XFileEndOfFileInformation,       /**< File end of file information class. */
  XFileAlternateNameInformation,   /**< File alternate name information class. */
  XFileStreamInformation,          /**< File stream information class. */
  XFileMountPartitionInformation,  /**< File mount partition information class. */
  XFileMountPartitionsInformation, /**< File mount partitions information class. */
  XFilePipeRemoteInformation,      /**< File pipe remote information class. */
  XFileSectorInformation,          /**< File sector information class. */
  XFileXctdCompressionInformation, /**< File XCTD compression information class. */
  XFileCompressionInformation,     /**< File compression information class. */
  XFileObjectIdInformation,        /**< File object ID information class. */
  XFileCompletionInformation,      /**< File completion information class. */
  XFileMoveClusterInformation,     /**< File move cluster information class. */
  XFileIoPriorityInformation,      /**< File I/O priority information class. */
  XFileReparsePointInformation,    /**< File reparse point information class. */
  XFileNetworkOpenInformation,     /**< File network open information class. */
  XFileAttributeTagInformation,    /**< File attribute tag information class. */
  XFileTrackingInformation,        /**< File tracking information class. */
  XFileMaximumInformation          /**< 1 past the last class. */
};

#pragma pack(push, 1)

/** Timestamps and attributes of a file. */
struct X_FILE_BASIC_INFORMATION {
  be_u64 creation_time;    /**< Offset 0x00. Creation time. */
  be_u64 last_access_time; /**< Offset 0x08. Last access time. */
  be_u64 last_write_time;  /**< Offset 0x10. Last write time. */
  be_u64 change_time;      /**< Offset 0x18. Last metadata change time. */
  be_u64 attributes;       /**< Offset 0x20. File attribute flags. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_BASIC_INFORMATION, 0x28);

/** Unique index of a file on its volume. */
struct X_FILE_INTERNAL_INFORMATION {
  be_u64 index_number; /**< Offset 0x00. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_INTERNAL_INFORMATION, 8);

/** Rename request for a file. */
struct X_FILE_RENAME_INFORMATION {
  be_u32 replace_existing;   /**< Offset 0x00. Nonzero to replace an existing target. */
  be_u32 root_dir_handle;    /**< Offset 0x04. Directory handle the target is relative to, or 0. */
  X_ANSI_STRING ansi_string; /**< Offset 0x08. Target path. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_RENAME_INFORMATION, 16);

/** Delete on close request for a file. */
struct X_FILE_DISPOSITION_INFORMATION {
  uint8_t delete_file; /**< Offset 0x00. Nonzero to delete the file on close. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_DISPOSITION_INFORMATION, 1);

/** Current byte position of a file. */
struct X_FILE_POSITION_INFORMATION {
  be_u64 current_byte_offset; /**< Offset 0x00. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_POSITION_INFORMATION, 8);

/** End of file position of a file. */
struct X_FILE_END_OF_FILE_INFORMATION {
  be_u64 end_of_file; /**< Offset 0x00. Size in bytes. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_END_OF_FILE_INFORMATION, 8);

/** Allocation size of a file. */
struct X_FILE_ALLOCATION_INFORMATION {
  be_u64 allocation_size; /**< Offset 0x00. Size in bytes. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_ALLOCATION_INFORMATION, 8);

/** Result of the XFileXctdCompressionInformation query. */
struct X_FILE_XCTD_COMPRESSION_INFORMATION {
  be_u32 unknown; /**< Offset 0x00. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_XCTD_COMPRESSION_INFORMATION, 4);

/** Association of a file with an I/O completion port. */
struct X_FILE_COMPLETION_INFORMATION {
  be_u32 handle; /**< Offset 0x00. I/O completion port handle. */
  be_u32 key;    /**< Offset 0x04. Completion key reported with each completion. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_COMPLETION_INFORMATION, 8);

/** Timestamps, sizes and attributes of a file. */
struct X_FILE_NETWORK_OPEN_INFORMATION {
  be_u64 creation_time;    /**< Offset 0x00. Creation time. */
  be_u64 last_access_time; /**< Offset 0x08. Last access time. */
  be_u64 last_write_time;  /**< Offset 0x10. Last write time. */
  be_u64 change_time;      /**< Offset 0x18. Last metadata change time. */
  be_u64 allocation_size;  /**< Offset 0x20. Size in bytes. */
  be_u64 end_of_file;      /**< Offset 0x28. Size in bytes. */
  be_u32 attributes;       /**< Offset 0x30. File attribute flags. */
  be_u32 pad;              /**< Offset 0x34. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_NETWORK_OPEN_INFORMATION, 56);

#pragma pack(pop)

}  // namespace runtime
}  // namespace rex
