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

#include <string>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>

namespace rex::system {

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_directory_information
class X_FILE_DIRECTORY_INFORMATION {
 public:
  // FILE_DIRECTORY_INFORMATION
  be<uint32_t> next_entry_offset;  // 0x0
  be<uint32_t> file_index;         // 0x4
  be<uint64_t> creation_time;      // 0x8
  be<uint64_t> last_access_time;   // 0x10
  be<uint64_t> last_write_time;    // 0x18
  be<uint64_t> change_time;        // 0x20
  be<uint64_t> end_of_file;        // 0x28 size in bytes
  be<uint64_t> allocation_size;    // 0x30
  be<uint32_t> attributes;         // 0x38 X_FILE_ATTRIBUTES
  be<uint32_t> file_name_length;   // 0x3C
  char file_name[1];               // 0x40

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

class XFile : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::File;

  XFile(KernelState* kernel_state, rex::filesystem::File* file, bool synchronous);
  ~XFile() override;

  rex::filesystem::Device* device() const { return file_->entry()->device(); }
  rex::filesystem::Entry* entry() const { return file_->entry(); }
  rex::filesystem::File* file() const { return file_; }
  uint32_t file_access() const { return file_->file_access(); }

  const std::string& path() const { return file_->entry()->path(); }
  const std::string& name() const { return file_->entry()->name(); }

  uint64_t position() const { return position_; }
  void set_position(uint64_t value) { position_ = value; }

  X_STATUS QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                          const std::string_view file_name, bool restart);

  // Don't do within the global critical region because invalidation callbacks
  // may be triggered (as per the usual rule of not doing I/O within the global
  // critical region).
  X_STATUS Read(uint32_t buffer_guess_address, uint32_t buffer_length, uint64_t byte_offset,
                uint32_t* out_bytes_read, uint32_t apc_context, bool notify_completion = true);

  X_STATUS ReadScatter(uint32_t segments_guest_address, uint32_t length, uint64_t byte_offset,
                       uint32_t* out_bytes_read, uint32_t apc_context);

  X_STATUS Write(uint32_t buffer_guess_address, uint32_t buffer_length, uint64_t byte_offset,
                 uint32_t* out_bytes_written, uint32_t apc_context);

  X_STATUS SetLength(size_t length);

  void RegisterIOCompletionPort(uint32_t key, object_ref<XIOCompletion> port);
  void RemoveIOCompletionPort(uint32_t key);

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XFile> Restore(KernelState* kernel_state, stream::ByteStream* stream);

  bool is_synchronous() const { return is_synchronous_; }

  void SetFindPattern(const std::string_view pattern) {
    find_engine_.SetRule(pattern);
    find_index_ = 0;
  }

  rex::filesystem::Entry* FindNext() {
    return file_->entry()->IterateChildren(find_engine_, &find_index_);
  }

 protected:
  void NotifyIOCompletionPorts(XIOCompletion::IONotification& notification);

  rex::thread::WaitHandle* GetWaitHandle() override { return async_event_.get(); }

 private:
  XFile();

  rex::filesystem::File* file_ = nullptr;
  std::unique_ptr<rex::thread::Event> async_event_ = nullptr;

  std::mutex completion_port_lock_;
  std::vector<std::pair<uint32_t, object_ref<XIOCompletion>>> completion_ports_;

  // TODO(benvanik): create flags, open state, etc.

  uint64_t position_ = 0;

  rex::filesystem::WildcardEngine find_engine_;
  size_t find_index_ = 0;

  bool is_synchronous_ = false;
};

}  // namespace rex::system
