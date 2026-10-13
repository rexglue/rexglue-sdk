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

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/filesystem.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/result.h>
#include <rex/runtime/error.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/io_completion_object.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

class IObjectTable;

/** Origin a seek distance is measured from, in the title move method values. */
enum class SeekOrigin : u8 {
  kBegin = 0,   /**< The start of the file. */
  kCurrent = 1, /**< The current position. */
  kEnd = 2,     /**< The end of the file. */
};

/** Guest layout of 1 entry of a directory listing, chained through next_entry_offset. */
class X_FILE_DIRECTORY_INFORMATION {
 public:
  be_u32 next_entry_offset;
  be_u32 file_index;
  be_u64 creation_time;
  be_u64 last_access_time;
  be_u64 last_write_time;
  be_u64 change_time;
  be_u64 end_of_file; /**< Size in bytes. */
  be_u64 allocation_size;
  be_u32 attributes; /**< X_FILE_ATTRIBUTES. */
  be_u32 file_name_length;
  char file_name[1];

  /**
   * Copies this entry and the entries chained after it into guest memory.
   *
   * @param base The host address of guest memory.
   * @param p The guest address of the first copy.
   */
  void Write(u8* base, u32 p) {
    u8* dst = base + p;
    u8* src = reinterpret_cast<u8*>(this);
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
REX_STATIC_ASSERT_SIZE(X_FILE_DIRECTORY_INFORMATION, 0x48);

/** The host state of a file or directory object. */
struct FileObject {
  /**
   * @param file_in The opened file or directory.
   * @param synchronous True when the file was opened for synchronous IO.
   */
  REX_RUNTIME_API FileObject(std::unique_ptr<filesystem::IFile> file_in, bool synchronous);

  /** Closes the IO completion port handles the file owns. */
  REX_RUNTIME_API ~FileObject();

  std::unique_ptr<filesystem::IFile> file; /**< Opened file or directory. */
  /** Auto reset event set when an asynchronous read or write completes, waited on by the handle. */
  std::unique_ptr<rex::platform::Event> async_event;
  mutable std::mutex file_lock;    /**< Guards position_, find and the reads and writes of file. */
  std::mutex completion_port_lock; /**< Guards completion_ports and port_table. */
  /** Completion key and port handle of each IO completion port the file posts to. */
  std::vector<std::pair<u32, X_HANDLE>> completion_ports;
  /** Table that owns the handles in completion_ports, set when the first port registers. */
  IObjectTable* port_table = nullptr;
  /** Directory listing a find call is walking, null before the first find. */
  std::unique_ptr<filesystem::IDirectory> find;
  bool is_synchronous = false; /**< True when the title opened the file for synchronous IO. */

  /**
   * Reads into the page sized segments of a guest segment array.
   *
   * @param runtime The runtime the call runs in.
   * @param segments_address The guest address of the segment array.
   * @param length The bytes to read.
   * @param byte_offset The file offset, or a negative value to read at the current position.
   * @param out_bytes_read Receives the bytes read, may be null.
   * @param apc_context The context reported to the completion ports.
   * @return The status of the first failed read, otherwise X_STATUS_SUCCESS.
   */
  REX_RUNTIME_API X_STATUS ReadScatter(Runtime& runtime, u32 segments_address, u32 length,
                                       u64 byte_offset, u32* out_bytes_read, u32 apc_context);

  /**
   * Reads into guest memory and notifies the IO completion ports of the file.
   *
   * @param runtime The runtime the call runs in.
   * @param buffer_address The guest address of the buffer.
   * @param buffer_length The bytes to read.
   * @param byte_offset The file offset, or the maximum value to read at the current position.
   * @param out_bytes_read Receives the bytes read, may be null.
   * @param apc_context The context reported to the completion ports.
   * @param notify_completion False to skip the completion ports and the async event.
   * @return The status of the read.
   */
  REX_RUNTIME_API X_STATUS Read(Runtime& runtime, u32 buffer_address, u32 buffer_length,
                                u64 byte_offset, u32* out_bytes_read, u32 apc_context,
                                bool notify_completion = true);

  /**
   * Writes from guest memory and notifies the IO completion ports of the file.
   *
   * @param runtime The runtime the call runs in.
   * @param buffer_address The guest address of the buffer.
   * @param buffer_length The bytes to write.
   * @param byte_offset The file offset, or the maximum value to write at the current position.
   * @param out_bytes_written Receives the bytes written, may be null.
   * @param apc_context The context reported to the completion ports.
   * @return The status of the write.
   */
  REX_RUNTIME_API X_STATUS Write(Runtime& runtime, u32 buffer_address, u32 buffer_length,
                                 u64 byte_offset, u32* out_bytes_written, u32 apc_context);

  /**
   * Writes the next directory entry that matches a pattern.
   *
   * @param runtime The runtime the call runs in.
   * @param out_info Receives the entry.
   * @param length The size of the output buffer.
   * @param file_name The pattern, empty to continue the current listing.
   * @param restart True to restart the current listing.
   * @return X_STATUS_SUCCESS, X_STATUS_NO_SUCH_FILE or X_STATUS_NO_MORE_FILES.
   */
  REX_RUNTIME_API X_STATUS QueryDirectory(Runtime& runtime, X_FILE_DIRECTORY_INFORMATION* out_info,
                                          size_t length, std::string_view file_name, bool restart);

  /**
   * Sets the length of the file.
   *
   * @param length The new length.
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_RUNTIME_API X_STATUS SetLength(u64 length);

  /**
   * Sets the allocation size of the file, which truncates only a file that is longer.
   *
   * @param size The allocation size in bytes.
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_RUNTIME_API X_STATUS SetAllocation(u64 size);

  /**
   * Moves the file inside its mount.
   *
   * @param file_system The file system that serves the file.
   * @param path The new path.
   * @param replace_existing True to replace a closed file at the new path.
   * @param root The directory the new path is relative to, or null.
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_RUNTIME_API X_STATUS Rename(filesystem::IFileSystem& file_system, std::string_view path,
                                  bool replace_existing, FileObject* root);

  /**
   * Moves the current position by a distance from an origin.
   *
   * @param distance The signed byte distance.
   * @param origin The origin the distance is measured from.
   * @return The new position.
   */
  REX_RUNTIME_API u64 Seek(i64 distance, SeekOrigin origin);

  /**
   * Sets the length of the file to the current position.
   *
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_RUNTIME_API X_STATUS SetLengthToPosition();

  /**
   * Sets the current position of the file.
   *
   * @param value The new position.
   */
  REX_RUNTIME_API void set_position(u64 value);

  /**
   * Reads the current position of the file.
   *
   * @return The position.
   */
  REX_RUNTIME_API u64 position();

  /**
   * Starts a directory listing.
   *
   * @param pattern The pattern to match, empty for every entry.
   */
  REX_RUNTIME_API void SetFindPattern(std::string_view pattern);

  /**
   * Returns the next entry of the current listing.
   *
   * @return The entry, empty when the listing is exhausted or absent.
   */
  REX_RUNTIME_API std::optional<filesystem::FileInfo> FindNext();

  /**
   * Associates an IO completion port with the file. The file owns a duplicate of the port handle.
   *
   * @param runtime The runtime the call runs in.
   * @param key The completion key reported with each notification.
   * @param port The handle of the IO completion port.
   */
  REX_RUNTIME_API void RegisterIOCompletionPort(Runtime& runtime, u32 key, X_HANDLE port);

 private:
  /**
   * Reads into guest memory while the caller holds file_lock.
   *
   * @param runtime The runtime the call runs in.
   * @param buffer_address The guest address of the buffer.
   * @param buffer_length The bytes to read.
   * @param byte_offset The file offset, or the maximum value to read at the current position.
   * @param out_bytes_read Receives the bytes read, may be null.
   * @param apc_context The context reported to the completion ports.
   * @param notify_completion False to skip the completion ports and the async event.
   * @return The status of the read.
   */
  X_STATUS ReadInternal(Runtime& runtime, u32 buffer_address, u32 buffer_length, u64 byte_offset,
                        u32* out_bytes_read, u32 apc_context, bool notify_completion);

  u64 position_ = 0; /**< Current byte offset used by reads and writes that pass no offset. */
};

}  // namespace rex::runtime
