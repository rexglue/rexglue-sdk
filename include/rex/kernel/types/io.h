/**
 * @file        kernel/types/io.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The file and volume information classes derive from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/assert.h>
#include <rex/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/string/text.h>
#include <rex/kernel/heap.h>

namespace rex::kernel {

#pragma pack(push, 4)

/**
 * File attribute flags. Hidden, system, temporary, compressed and encrypted are NT bits the XDK
 * does not list.
 */
enum X_FILE_ATTRIBUTES : u16 {
  kXFileAttributeNone = 0x0000,
  kXFileAttributeReadOnly = 0x0001,
  kXFileAttributeHidden = 0x0002,
  kXFileAttributeSystem = 0x0004,
  kXFileAttributeDirectory = 0x0010,
  kXFileAttributeArchive = 0x0020,
  kXFileAttributeDevice = 0x0040, /**< Reserved, titles do not use it. */
  kXFileAttributeNormal = 0x0080, /**< The file has no other attribute, valid only on its own. */
  kXFileAttributeTemporary = 0x0100,
  kXFileAttributeCompressed = 0x0800,
  kXFileAttributeEncrypted = 0x4000,
};

/** Guest layout of an overlapped IO block. */
struct XOVERLAPPED {
  be_u32 result;
  be_u32 length;
  be_u32 context;            /**< Reserved for system use. */
  be_u32 event;              /**< Guest handle of the event signaled on completion. */
  be_u32 completion_routine; /**< Guest address of the completion routine, 0 for none. */
  be_u32 completion_context;
  be_u32 extended_error;
};

REX_STATIC_ASSERT_SIZE(XOVERLAPPED, 0x1C);

/**
 * Publishes the result of an overlapped call.
 *
 * @param ptr The XOVERLAPPED block.
 */
inline void XOverlappedSetResult(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->result.Store(value, std::memory_order_release);
}

/**
 * Returns the result of an overlapped call.
 *
 * @param ptr The XOVERLAPPED block.
 */
inline u32 XOverlappedGetResult(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->result.Load(std::memory_order_acquire);
}

inline void XOverlappedSetLength(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->length = value;
}

inline u32 XOverlappedGetContext(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->context;
}

inline void XOverlappedSetContext(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->context = value;
}

/**
 * Returns the event of an overlapped call, or 0 when there is none.
 *
 * @param ptr The XOVERLAPPED block.
 */
inline X_HANDLE XOverlappedGetEvent(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->event;
}

/**
 * Returns the completion routine of an overlapped call, or 0 when there is none.
 *
 * @param ptr The XOVERLAPPED block.
 */
inline u32 XOverlappedGetCompletionRoutine(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->completion_routine;
}

inline void XOverlappedSetExtendedError(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->extended_error = value;
}

/** A counted 8 bit string in guest memory. */
struct X_ANSI_STRING {
  be_u16 length;         /**< Used bytes of the string, without a terminator. */
  be_u16 maximum_length; /**< Capacity in bytes of the buffer at pointer. */
  be_u32 pointer;        /**< Guest address of the characters. */

  void Reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_ANSI_STRING, 8);

/** A counted 16 bit string in guest memory. */
struct X_UNICODE_STRING {
  be_u16 length;         /**< Length of the text in bytes. */
  be_u16 maximum_length; /**< Capacity of the buffer in bytes. */
  be_u32 pointer;        /**< Guest address of the text buffer. */

  void Reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_UNICODE_STRING, 8);

struct X_IO_STATUS_BLOCK {
  union {
    BE<X_STATUS> status;
    be_u32 pointer; /**< Shares its dword with status. */
  };
  /** Result that depends on the call, such as the bytes transferred or the file action. */
  be_u32 information;
};

struct X_OBJECT_ATTRIBUTES {
  be_u32 root_directory; /**< Handle of the directory the name is relative to, 0 for none. */
  be_u32 name_ptr;       /**< PANSI_STRING. */
  be_u32 attributes;
};
REX_STATIC_ASSERT_SIZE(X_OBJECT_ATTRIBUTES, 0xC);

#pragma pack(pop)

/** Object attribute that opens an existing object of the same type when the name is taken. */
inline constexpr u32 kObjOpenIf = 0x80;

/** File information class selecting the layout of a query or set request. */
enum X_FILE_INFORMATION_CLASS : u8 {
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
  XFileMaximumInformation /**< 1 past the last class. */
};

#pragma pack(push, 1)

struct X_FILE_BASIC_INFORMATION {
  be_u64 creation_time;
  be_u64 last_access_time;
  be_u64 last_write_time;
  be_u64 change_time;
  be_u64 attributes;
};
REX_STATIC_ASSERT_SIZE(X_FILE_BASIC_INFORMATION, 0x28);

/** Unique index of a file on its volume. */
struct X_FILE_INTERNAL_INFORMATION {
  be_u64 index_number; /**< Unique index of the file, a hash of its path. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_INTERNAL_INFORMATION, 8);

struct X_FILE_RENAME_INFORMATION {
  be_u32 replace_existing; /**< Nonzero to replace an existing target. */
  be_u32 root_dir_handle;  /**< Directory handle the target is relative to, or 0. */
  X_ANSI_STRING ansi_string;
};
REX_STATIC_ASSERT_SIZE(X_FILE_RENAME_INFORMATION, 16);

struct X_FILE_DISPOSITION_INFORMATION {
  u8 delete_file; /**< Nonzero to delete the file on close. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_DISPOSITION_INFORMATION, 1);

struct X_FILE_POSITION_INFORMATION {
  be_u64 current_byte_offset;
};
REX_STATIC_ASSERT_SIZE(X_FILE_POSITION_INFORMATION, 8);

struct X_FILE_END_OF_FILE_INFORMATION {
  be_u64 end_of_file; /**< Size in bytes of the file data. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_END_OF_FILE_INFORMATION, 8);

struct X_FILE_ALLOCATION_INFORMATION {
  be_u64 allocation_size; /**< Size in bytes the file occupies on disk. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_ALLOCATION_INFORMATION, 8);

/** Result of the XFileXctdCompressionInformation query. */
struct X_FILE_XCTD_COMPRESSION_INFORMATION {
  be_u32 unknown;
};
REX_STATIC_ASSERT_SIZE(X_FILE_XCTD_COMPRESSION_INFORMATION, 4);

/** Association of a file with an IO completion port. */
struct X_FILE_COMPLETION_INFORMATION {
  be_u32 handle;
  be_u32 key;
};
REX_STATIC_ASSERT_SIZE(X_FILE_COMPLETION_INFORMATION, 8);

struct X_FILE_NETWORK_OPEN_INFORMATION {
  be_u64 creation_time;
  be_u64 last_access_time;
  be_u64 last_write_time;
  be_u64 change_time;
  be_u64 allocation_size; /**< Size in bytes the file occupies on disk. */
  be_u64 end_of_file;     /**< Size in bytes of the file data. */
  be_u32 attributes;
  be_u32 pad;
};
REX_STATIC_ASSERT_SIZE(X_FILE_NETWORK_OPEN_INFORMATION, 56);

#pragma pack(pop)

/** Volume information class selecting the layout of a query request. */
enum X_FILE_FS_INFORMATION_CLASS : u8 {
  XFileFsVolumeInformation = 1, /**< Volume label and serial number. */
  XFileFsSizeInformation = 3,   /**< Volume size and allocation unit geometry. */
  XFileFsDeviceInformation,
  XFileFsAttributeInformation = 5, /**< File system name and capabilities. */
};

/** Device type reported for a volume. */
// NOLINTNEXTLINE(performance-enum-size): guest memory field width
enum X_FILE_DEVICE_TYPE : u32 {
  kFileDeviceUnknown = 0x00000022, /**< Device type the volume device query reports. */
  kFileDeviceEHSTOR = 0x00000046,
};

#pragma pack(push, 1)

struct X_FILE_FS_VOLUME_INFORMATION {
  be_u64 creation_time;
  be_u32 serial_number;
  be_u32 label_length; /**< Label length in bytes. */
  u8 supports_objects;
  char label[1]; /**< First character of the label. */
  u8 pad[6];
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_VOLUME_INFORMATION, 24);

struct X_FILE_FS_SIZE_INFORMATION {
  be_u64 total_allocation_units;
  be_u64 available_allocation_units; /**< Allocation units available to the caller. */
  be_u32 sectors_per_allocation_unit;
  be_u32 bytes_per_sector;
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_SIZE_INFORMATION, 24);

struct X_FILE_FS_ATTRIBUTE_INFORMATION {
  be_u32 attributes;
  be_i32 component_name_max_length; /**< Longest path component in characters. */
  be_u32 name_length;               /**< Name length in bytes. */
  char name[1];                     /**< First character of the file system name. */
  u8 pad[3];
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_ATTRIBUTE_INFORMATION, 16);

struct X_FILE_FS_DEVICE_INFORMATION {
  BE<X_FILE_DEVICE_TYPE> device_type;
  be_u32 characteristics;
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_DEVICE_INFORMATION, 8);

#pragma pack(pop)

}  // namespace rex::kernel

namespace rex {
namespace kernel {

/**
 * Returns the characters of a guest ANSI string without copying, empty when the string is null or
 * has no length.
 *
 * @param ansi_string The guest string, may be null.
 */
inline std::string_view TranslateAnsiString(const kernel::GuestAddressSpace* memory,
                                            const X_ANSI_STRING* ansi_string) {
  if (!ansi_string || !ansi_string->length) {
    return "";
  }
  return std::string_view(memory->TranslateVirtual<const char*>(ansi_string->pointer),
                          ansi_string->length);
}

/**
 * Returns the text of a guest ANSI string with surrounding whitespace removed.
 *
 * @param ansi_string The guest string, may be null.
 */
inline std::string TranslateAnsiPath(const kernel::GuestAddressSpace* memory,
                                     const X_ANSI_STRING* ansi_string) {
  return rex::string::TrimString(std::string(TranslateAnsiString(memory, ansi_string)));
}

/**
 * Returns the characters of the guest ANSI string at an address without copying, empty when the
 * address is 0.
 *
 * @param guest_address The guest address of the string structure, 0 for none.
 */
inline std::string_view TranslateAnsiStringAddress(const kernel::GuestAddressSpace* memory,
                                                   u32 guest_address) {
  if (!guest_address) {
    return "";
  }
  return TranslateAnsiString(memory, memory->TranslateVirtual<const X_ANSI_STRING*>(guest_address));
}

/**
 * Converts a guest UTF-16 string to host byte order.
 *
 * @param unicode_string The guest string, may be null.
 * @return The text, empty when the string is null or has no length.
 */
inline std::u16string TranslateUnicodeString(const kernel::GuestAddressSpace* memory,
                                             const X_UNICODE_STRING* unicode_string) {
  if (!unicode_string) {
    return u"";
  }
  u16 length = unicode_string->length;
  if (!length) {
    return u"";
  }
  auto src = memory->TranslateVirtual<const be_u16*>(unicode_string->pointer);
  std::u16string result;
  result.reserve(length);
  for (u16 i = 0; i < length / 2; ++i) {
    result.push_back(src[i]);
  }
  return result;
}

}  // namespace kernel
}  // namespace rex
