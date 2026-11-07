/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/file_object.h
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

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/assert.h>
#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/directory.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/file_system.h>
#include <rex/platform/thread.h>
#include <rex/utility.h>
#include <rex/platform/sync.h>
#include <rex/result.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/object/object.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

class IObjectTable;

enum class SeekOrigin : u8 {
  kBegin = 0,
  kCurrent = 1,
  kEnd = 2,
};

/** Guest layout of one entry of a directory listing, chained through next_entry_offset. */
class X_FILE_DIRECTORY_INFORMATION {
 public:
  be_u32 next_entry_offset; /**< Offset in bytes from this entry to the next, 0 for the last. */
  be_u32 file_index;
  be_u64 creation_time;
  be_u64 last_access_time;
  be_u64 last_write_time;
  be_u64 change_time;
  be_u64 end_of_file;
  be_u64 allocation_size; /**< Size in bytes the file occupies on disk. */
  be_u32 attributes;
  be_u32 file_name_length; /**< Length of file_name in bytes. */
  char file_name[1];       /**< First character of the name, which continues past the struct. */

  /**
   * Copies this entry and the entries chained after it into guest memory.
   *
   * @param guest_address The guest address of the first copy.
   */
  void Write(u8* base, u32 guest_address) {
    u8* dst = base + guest_address;
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

/** Interface XAM uses to operate on an open file or directory. */
class IOpenedFile {
 public:
  virtual ~IOpenedFile() = default;

  /**
   * Reads into guest memory and notifies the IO completion ports of the file.
   *
   * @param byte_offset The file offset, or the maximum value to read at the current position.
   * @param out_bytes_read Receives the bytes read, may be null.
   * @param apc_context The context reported to the completion ports.
   * @param notify_completion False to skip the completion ports and the async event.
   */
  virtual X_STATUS Read(u32 buffer_address, u32 buffer_length, u64 byte_offset, u32* out_bytes_read,
                        u32 apc_context, bool notify_completion = true) = 0;

  /**
   * Writes from guest memory and notifies the IO completion ports of the file.
   *
   * @param byte_offset The file offset, or the maximum value to write at the current position.
   * @param out_bytes_written Receives the bytes written, may be null.
   * @param apc_context The context reported to the completion ports.
   */
  virtual X_STATUS Write(u32 buffer_address, u32 buffer_length, u64 byte_offset,
                         u32* out_bytes_written, u32 apc_context) = 0;

  /**
   * Moves the position of the file.
   *
   * @param distance The signed byte distance.
   * @param origin The origin the distance is measured from.
   */
  virtual u64 Seek(i64 distance, SeekOrigin origin) = 0;

  /**
   * Sets the length of the file to the current position.
   *
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  virtual X_STATUS SetLengthToPosition() = 0;

  /**
   * Starts a directory listing.
   *
   * @param pattern The pattern to match, empty for every entry.
   */
  virtual void SetFindPattern(std::string_view pattern) = 0;

  /**
   * @return The entry, empty when the listing is exhausted or absent.
   */
  virtual std::optional<filesystem::FileStat> FindNext() = 0;

  virtual u64 GetSize() = 0;

  virtual std::string GetAbsolutePath() = 0;
};

struct FileObject : IOpenedFile {
  /**
   * @param synchronous True when the file was opened for synchronous IO.
   */
  REX_KERNEL_API FileObject(std::unique_ptr<filesystem::IFile> file_in, bool synchronous);

  /** Closes the IO completion port handles the file owns. */
  REX_KERNEL_API ~FileObject();

  std::unique_ptr<filesystem::IFile> file;
  /** Auto reset event set when an asynchronous read or write completes, waited on by the handle. */
  std::unique_ptr<rex::platform::Event> async_event;
  mutable std::mutex file_lock;    /**< Guards position_, find and the reads and writes of file. */
  std::mutex completion_port_lock; /**< Guards completion_ports and port_table. */
  /** Completion key and port handle of each IO completion port the file posts to. */
  std::vector<std::pair<u32, X_HANDLE>> completion_ports;
  /** Table that owns the handles in completion_ports, set when the first port registers. */
  IObjectTable* port_table = nullptr;
  /** Directory listing a find call is walking, null before the first find. */
  PropagateConst<std::unique_ptr<filesystem::IDirectory>> find;
  bool is_synchronous = false;
  /** The runtime the interface Read and Write run in, set when the table creates the file. */
  TitleRuntime* owner = nullptr;

  /**
   * Reads into the page sized segments of a guest segment array.
   *
   * @param segments_address The guest address of the segment array.
   * @param byte_offset The file offset, or the maximum value to read at the current position.
   * @param out_bytes_read Receives the bytes read, may be null.
   * @param apc_context The context reported to the completion ports.
   * @return The status of the first failed read, otherwise X_STATUS_SUCCESS.
   */
  REX_KERNEL_API X_STATUS ReadScatter(TitleRuntime& runtime, u32 segments_address, u32 length,
                                      u64 byte_offset, u32* out_bytes_read, u32 apc_context);

  /**
   * Reads into guest memory and notifies the IO completion ports of the file.
   *
   * @param byte_offset The file offset, or the maximum value to read at the current position.
   * @param out_bytes_read Receives the bytes read, may be null.
   * @param apc_context The context reported to the completion ports.
   * @param notify_completion False to skip the completion ports and the async event.
   */
  REX_KERNEL_API X_STATUS Read(TitleRuntime& runtime, u32 buffer_address, u32 buffer_length,
                               u64 byte_offset, u32* out_bytes_read, u32 apc_context,
                               bool notify_completion = true);

  X_STATUS Read(u32 buffer_address, u32 buffer_length, u64 byte_offset, u32* out_bytes_read,
                u32 apc_context, bool notify_completion = true) override {
    return Read(*owner, buffer_address, buffer_length, byte_offset, out_bytes_read, apc_context,
                notify_completion);
  }

  /**
   * Writes from guest memory and notifies the IO completion ports of the file.
   *
   * @param byte_offset The file offset, or the maximum value to write at the current position.
   * @param out_bytes_written Receives the bytes written, may be null.
   * @param apc_context The context reported to the completion ports.
   */
  REX_KERNEL_API X_STATUS Write(TitleRuntime& runtime, u32 buffer_address, u32 buffer_length,
                                u64 byte_offset, u32* out_bytes_written, u32 apc_context);

  X_STATUS Write(u32 buffer_address, u32 buffer_length, u64 byte_offset, u32* out_bytes_written,
                 u32 apc_context) override {
    return Write(*owner, buffer_address, buffer_length, byte_offset, out_bytes_written,
                 apc_context);
  }

  /**
   * Writes the next directory entry that matches a pattern.
   *
   * @param out_info Receives the entry.
   * @param file_name The pattern, empty to continue the current listing.
   * @return X_STATUS_SUCCESS; X_STATUS_NO_SUCH_FILE when this call started the listing and nothing
   *         matches; X_STATUS_NO_MORE_FILES when a started listing has no entry left;
   *         X_STATUS_BUFFER_OVERFLOW when the name does not fit.
   */
  REX_KERNEL_API X_STATUS QueryDirectory(TitleRuntime& runtime,
                                         X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                                         std::string_view file_name, bool restart);

  /**
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_KERNEL_API X_STATUS SetLength(u64 length);

  /**
   * Sets the allocation size of the file, which truncates only a file that is longer.
   *
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_KERNEL_API X_STATUS SetAllocation(u64 size);

  /**
   * Moves the file inside its mount.
   *
   * @param file_system The file system that serves the file.
   * @param replace_existing True to replace a closed file at the new path.
   * @param root The directory the new path is relative to, or null.
   * @return X_STATUS_SUCCESS, or the mapped filesystem error.
   */
  REX_KERNEL_API X_STATUS Rename(filesystem::IFileSystem& file_system, std::string_view path,
                                 bool replace_existing, FileObject* root);

  REX_KERNEL_API u64 Seek(i64 distance, SeekOrigin origin) override;

  REX_KERNEL_API X_STATUS SetLengthToPosition() override;

  REX_KERNEL_API void SetPosition(u64 value);

  REX_KERNEL_API u64 GetPosition();

  REX_KERNEL_API void SetFindPattern(std::string_view pattern) override;

  REX_KERNEL_API std::optional<filesystem::FileStat> FindNext() override;

  u64 GetSize() override { return file->GetInfo().size; }

  std::string GetAbsolutePath() override { return file->GetInfo().absolute_path; }

  /**
   * Associates an IO completion port with the file. The file owns a duplicate of the port handle.
   *
   * @param key The completion key reported with each notification.
   * @param port The handle of the IO completion port.
   */
  REX_KERNEL_API void RegisterIOCompletionPort(TitleRuntime& runtime, u32 key, X_HANDLE port);

 private:
  X_STATUS ReadInternal(TitleRuntime& runtime, u32 buffer_address, u32 buffer_length,
                        u64 byte_offset, u32* out_bytes_read, u32 apc_context,
                        bool notify_completion);

  u64 position_ = 0; /**< Current byte offset used by reads and writes that pass no offset. */
};

}  // namespace rex::kernel
