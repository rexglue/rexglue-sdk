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

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/filesystem.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/result.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xiocompletion.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

class IObjectTable;

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_directory_information
class X_FILE_DIRECTORY_INFORMATION {
 public:
  // FILE_DIRECTORY_INFORMATION
  be_u32 next_entry_offset;  // 0x0
  be_u32 file_index;         // 0x4
  be_u64 creation_time;      // 0x8
  be_u64 last_access_time;   // 0x10
  be_u64 last_write_time;    // 0x18
  be_u64 change_time;        // 0x20
  be_u64 end_of_file;        // 0x28 size in bytes
  be_u64 allocation_size;    // 0x30
  be_u32 attributes;         // 0x38 X_FILE_ATTRIBUTES
  be_u32 file_name_length;   // 0x3C
  char file_name[1];         // 0x40

  void Write(uint8_t* base, uint32_t p) {
    uint8_t* dst = base + p;
    uint8_t* src = reinterpret_cast<uint8_t*>(this);
    X_FILE_DIRECTORY_INFORMATION* right;
    do {
      auto left = reinterpret_cast<X_FILE_DIRECTORY_INFORMATION*>(dst);
      right = reinterpret_cast<X_FILE_DIRECTORY_INFORMATION*>(src);
      left->next_entry_offset = right->next_entry_offset;
      left->file_index = right->file_index;
      left->creation_time = right->creation_time;
      left->last_access_time = right->last_access_time;
      left->last_write_time = right->last_write_time;
      left->change_time = right->change_time;
      left->end_of_file = right->end_of_file;
      left->allocation_size = right->allocation_size;
      left->attributes = right->attributes;
      left->file_name_length = right->file_name_length;
      std::memcpy(left->file_name, right->file_name, right->file_name_length);

      dst += right->next_entry_offset;
      src += right->next_entry_offset;
    } while (right->next_entry_offset != 0);
  }
};

/** The host state of a file or directory object. */
struct FileObject {
  /**
   * @param file_in The opened file or directory.
   * @param synchronous True when the file was opened for synchronous IO.
   */
  REX_RUNTIME_API FileObject(std::unique_ptr<filesystem::IFile> file_in, bool synchronous);

  /** Closes the IO completion port handles the file owns. */
  REX_RUNTIME_API ~FileObject();

  std::unique_ptr<filesystem::IFile> file;
  std::unique_ptr<rex::platform::Event> async_event;
  mutable std::mutex file_lock;
  std::mutex completion_port_lock;
  std::vector<std::pair<u32, X_HANDLE>> completion_ports;
  IObjectTable* port_table = nullptr;
  u64 position = 0;
  std::unique_ptr<filesystem::IDirectory> find;
  bool is_synchronous = false;
};

/**
 * Writes the next directory entry that matches a pattern.
 *
 * @param runtime The runtime the call runs in.
 * @param file The directory.
 * @param out_info Receives the entry.
 * @param length The size of the output buffer.
 * @param file_name The pattern, empty to continue the current listing.
 * @param restart True to restart the current listing.
 * @return X_STATUS_SUCCESS, X_STATUS_NO_SUCH_FILE or X_STATUS_NO_MORE_FILES.
 */
REX_RUNTIME_API X_STATUS FileQueryDirectory(Runtime& runtime, FileObject& file,
                                            X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                                            std::string_view file_name, bool restart);

/**
 * Reads into guest memory and notifies the IO completion ports of the file.
 *
 * @param runtime The runtime the call runs in.
 * @param file The file.
 * @param buffer_guess_address The guest address of the buffer.
 * @param buffer_length The bytes to read.
 * @param byte_offset The file offset, or the maximum value to read at the current position.
 * @param out_bytes_read Receives the bytes read, may be null.
 * @param apc_context The context reported to the completion ports.
 * @param notify_completion False to skip the completion ports and the async event.
 * @return The status of the read.
 */
REX_RUNTIME_API X_STATUS FileRead(Runtime& runtime, FileObject& file, u32 buffer_guess_address,
                                  u32 buffer_length, u64 byte_offset, u32* out_bytes_read,
                                  u32 apc_context, bool notify_completion = true);

/**
 * Reads into the page sized segments of a guest segment array.
 *
 * @param runtime The runtime the call runs in.
 * @param file The file.
 * @param segments_guest_address The guest address of the segment array.
 * @param length The bytes to read.
 * @param byte_offset The file offset, or a negative value to read at the current position.
 * @param out_bytes_read Receives the bytes read, may be null.
 * @param apc_context The context reported to the completion ports.
 * @return The status of the first failed read, otherwise X_STATUS_SUCCESS.
 */
REX_RUNTIME_API X_STATUS FileReadScatter(Runtime& runtime, FileObject& file,
                                         u32 segments_guest_address, u32 length, u64 byte_offset,
                                         u32* out_bytes_read, u32 apc_context);

/**
 * Writes from guest memory and notifies the IO completion ports of the file.
 *
 * @param runtime The runtime the call runs in.
 * @param file The file.
 * @param buffer_guess_address The guest address of the buffer.
 * @param buffer_length The bytes to write.
 * @param byte_offset The file offset, or the maximum value to write at the current position.
 * @param out_bytes_written Receives the bytes written, may be null.
 * @param apc_context The context reported to the completion ports.
 * @return The status of the write.
 */
REX_RUNTIME_API X_STATUS FileWrite(Runtime& runtime, FileObject& file, u32 buffer_guess_address,
                                   u32 buffer_length, u64 byte_offset, u32* out_bytes_written,
                                   u32 apc_context);

/**
 * Sets the length of the file.
 *
 * @param file The file.
 * @param length The new length.
 * @return X_STATUS_SUCCESS, or the mapped filesystem error.
 */
REX_RUNTIME_API X_STATUS FileSetLength(FileObject& file, u64 length);

/**
 * Sets the allocation size of the file, which truncates only a file that is longer.
 *
 * @param file The file.
 * @param size The allocation size in bytes.
 * @return X_STATUS_SUCCESS, or the mapped filesystem error.
 */
REX_RUNTIME_API X_STATUS FileSetAllocation(FileObject& file, u64 size);

/**
 * Moves the file inside its mount.
 *
 * @param file_system The file system that serves the file.
 * @param file The file.
 * @param path The new path.
 * @param replace_existing True to replace a closed file at the new path.
 * @param root The directory the new path is relative to, or null.
 * @return X_STATUS_SUCCESS, or the mapped filesystem error.
 */
REX_RUNTIME_API X_STATUS FileRename(filesystem::IFileSystem& file_system, FileObject& file,
                                    std::string_view path, bool replace_existing, FileObject* root);

/**
 * Associates an IO completion port with the file. The file owns a duplicate of the port handle.
 *
 * @param runtime The runtime the call runs in.
 * @param file The file.
 * @param key The completion key reported with each notification.
 * @param port The handle of the IO completion port.
 */
REX_RUNTIME_API void FileRegisterIOCompletionPort(Runtime& runtime, FileObject& file, u32 key,
                                                  X_HANDLE port);

/**
 * Reads the current position of the file.
 *
 * @param file The file.
 * @return The position.
 */
REX_RUNTIME_API u64 FilePosition(FileObject& file);

/**
 * Sets the current position of the file.
 *
 * @param file The file.
 * @param value The new position.
 */
REX_RUNTIME_API void FileSetPosition(FileObject& file, u64 value);

/**
 * Starts a directory listing.
 *
 * @param file The directory.
 * @param pattern The pattern to match, empty for every entry.
 */
REX_RUNTIME_API void FileSetFindPattern(FileObject& file, std::string_view pattern);

/**
 * Returns the next entry of the current listing.
 *
 * @param file The directory.
 * @return The entry, empty when the listing is exhausted or absent.
 */
REX_RUNTIME_API std::optional<filesystem::FileInfo> FileFindNext(FileObject& file);

}  // namespace rex::runtime
