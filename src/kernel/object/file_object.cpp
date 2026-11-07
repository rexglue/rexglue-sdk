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

#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/file_system.h>
#include <rex/logging/log_category.h>
#include <rex/string/text.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/object/io_completion_object.h>
#include <rex/kernel/object/file_object.h>
#include <rex/platform/sync.h>

#include <span>
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

void NotifyIOCompletionPorts(TitleRuntime& runtime, FileObject& file,
                             IONotification& notification) {
  std::vector<std::pair<u32, X_HANDLE>> ports;
  {
    std::lock_guard<std::mutex> lock(file.completion_port_lock);
    ports = file.completion_ports;
  }

  for (const auto& port : ports) {
    auto pin = runtime.GetObjectTable().Get<IOCompletionObject>(port.second);
    if (pin) {
      notification.key = port.first;
      pin->Queue(notification);
    }
  }
}

}  // namespace

X_STATUS FileObject::ReadInternal(TitleRuntime& runtime, u32 buffer_address, u32 buffer_length,
                                  u64 byte_offset, u32* out_bytes_read, u32 apc_context,
                                  bool notify_completion) {
  auto* memory = StateOf(runtime).GetAddressSpace();
  if (byte_offset == static_cast<u64>(-1)) {
    byte_offset = position_;
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  if (buffer_length) {
    if (UINT32_MAX - buffer_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      u32 buffer_guest_high_address = buffer_address + buffer_length - 1;
      rex::kernel::BaseHeap* buffer_start_heap = memory->LookupHeap(buffer_address);
      const rex::kernel::BaseHeap* buffer_end_heap = memory->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->GetHeapType() == kernel::HeapType::kGuestPhysical) !=
              (buffer_end_heap->GetHeapType() == kernel::HeapType::kGuestPhysical) ||
          (buffer_start_heap->GetHeapType() == kernel::HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        rex::kernel::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->GetHeapType() == kernel::HeapType::kGuestPhysical
                ? static_cast<rex::kernel::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap &&
            buffer_physical_heap->QueryRangeAccess(buffer_address, buffer_guest_high_address) !=
                memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          auto read = file->Read(
              std::span<u8>(buffer_physical_heap
                                ? memory->TranslatePhysical(
                                      buffer_physical_heap->GetPhysicalAddress(buffer_address))
                                : memory->TranslateVirtual(buffer_address),
                            buffer_length),
              byte_offset);
          if (read) {
            bytes_read = *read;
          } else {
            result = ToXStatus(read.error());
          }
          if (XSUCCEEDED(result)) {
            if (buffer_physical_heap) {
              buffer_physical_heap->TriggerCallbacks(buffer_address, buffer_length, true, true);
            }

            if (byte_offset) {
              position_ = byte_offset;
            }
            position_ += bytes_read;
          }
        }
      }
    }
  }

  if (out_bytes_read) {
    *out_bytes_read = static_cast<u32>(bytes_read);
  }

  if (notify_completion) {
    IONotification notify;
    notify.apc_context = apc_context;
    notify.num_bytes = static_cast<u32>(bytes_read);
    notify.status = result;

    NotifyIOCompletionPorts(runtime, *this, notify);

    async_event->Set();
  }

  return result;
}

FileObject::FileObject(std::unique_ptr<filesystem::IFile> file_in, bool synchronous)
    : file(std::move(file_in)), is_synchronous(synchronous) {
  async_event = rex::platform::Event::CreateAutoResetEvent(false);
  REX_ASSERT(async_event != nullptr);
}

FileObject::~FileObject() {
  if (port_table == nullptr) {
    return;
  }
  for (const auto& port : completion_ports) {
    CloseOwned(*port_table, port.second);
  }
}

X_STATUS FileObject::QueryDirectory(TitleRuntime&, X_FILE_DIRECTORY_INFORMATION* out_info,
                                    size_t length, std::string_view file_name, bool restart) {
  std::lock_guard<std::mutex> lock(file_lock);
  REX_ASSERT(out_info != nullptr);

  std::optional<filesystem::FileStat> entry;

  if (!file_name.empty()) {
    REX_ASSERT(rex::string::utf8_find_any_of(file_name, "\\") == std::string_view::npos);
    SetFindPattern(file_name);
    entry = FindNext();
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    const bool started = !find;
    if (started) {
      SetFindPattern({});
    } else if (restart) {
      find->Restart();
    }
    entry = FindNext();
    if (!entry) {
      return started ? X_STATUS_NO_SUCH_FILE : X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<u8*>(out_info) + length;
  const std::string& entry_name = entry->name;
  if (reinterpret_cast<u8*>(&out_info->file_name[0]) + entry_name.size() > end) {
    REX_ASSERT_ALWAYS("Buffer overflow?");
    return X_STATUS_BUFFER_OVERFLOW;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<u32>(find->GetPosition());
  out_info->creation_time = entry->create_time;
  out_info->last_access_time = entry->access_time;
  out_info->last_write_time = entry->write_time;
  out_info->change_time = entry->write_time;
  out_info->end_of_file = entry->size;
  out_info->allocation_size = entry->allocation_size;
  out_info->attributes = std::to_underlying(entry->attributes);
  out_info->file_name_length = static_cast<u32>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS FileObject::Read(TitleRuntime& runtime, u32 buffer_address, u32 buffer_length,
                          u64 byte_offset, u32* out_bytes_read, u32 apc_context,
                          bool notify_completion) {
  std::lock_guard<std::mutex> lock(file_lock);
  return ReadInternal(runtime, buffer_address, buffer_length, byte_offset, out_bytes_read,
                      apc_context, notify_completion);
}

X_STATUS FileObject::ReadScatter(TitleRuntime& runtime, u32 segments_address, u32 length,
                                 u64 byte_offset, u32* out_bytes_read, u32 apc_context) {
  std::lock_guard<std::mutex> lock(file_lock);
  X_STATUS result = X_STATUS_SUCCESS;

  be_u32* segments = reinterpret_cast<be_u32*>(
      StateOf(runtime).GetAddressSpace()->TranslateVirtual(segments_address));

  u32 page_size = kPageSize4KiB;

  u32 read_total = 0;
  u32 read_remain = length;
  while (read_remain) {
    u32 read_length = read_remain;
    u32 read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    u32 bytes_read = 0;
    result = ReadInternal(
        runtime, read_buffer, read_length,
        byte_offset ? ((byte_offset != ~u64{0} && byte_offset != ~u64{1}) ? byte_offset + read_total
                                                                          : byte_offset)
                    : -1,
        &bytes_read, apc_context, false);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = read_total;
  }

  IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = read_total;
  notify.status = result;

  NotifyIOCompletionPorts(runtime, *this, notify);

  async_event->Set();

  return result;
}

X_STATUS FileObject::Write(TitleRuntime& runtime, u32 buffer_address, u32 buffer_length,
                           u64 byte_offset, u32* out_bytes_written, u32 apc_context) {
  std::lock_guard<std::mutex> lock(file_lock);
  if (byte_offset == static_cast<u64>(-1)) {
    byte_offset = position_;
  }

  size_t bytes_written = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  auto written = file->Write(
      std::span<const u8>(StateOf(runtime).GetAddressSpace()->TranslateVirtual(buffer_address),
                          buffer_length),
      byte_offset);
  if (written) {
    bytes_written = *written;
  } else {
    result = ToXStatus(written.error());
  }
  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
  }

  IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = static_cast<u32>(bytes_written);
  notify.status = result;

  NotifyIOCompletionPorts(runtime, *this, notify);

  if (out_bytes_written) {
    *out_bytes_written = static_cast<u32>(bytes_written);
  }

  async_event->Set();
  return result;
}

X_STATUS FileObject::SetLength(u64 length) {
  std::lock_guard<std::mutex> lock(file_lock);
  auto status = file->SetLength(length);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_IMPLEMENTED);
}

X_STATUS FileObject::SetAllocation(u64 size) {
  std::lock_guard<std::mutex> lock(file_lock);
  file->Refresh();
  if (size >= file->GetInfo().size) {
    return X_STATUS_SUCCESS;
  }
  auto status = file->SetLength(size);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_IMPLEMENTED);
}

X_STATUS FileObject::Rename(filesystem::IFileSystem& file_system, std::string_view path,
                            bool replace_existing, FileObject* root) {
  std::lock_guard<std::mutex> lock(file_lock);
  auto status =
      file_system.Rename(*file, path, replace_existing, root ? root->file.get() : nullptr);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_SUPPORTED);
}

u64 FileObject::Seek(i64 distance, SeekOrigin origin) {
  std::lock_guard<std::mutex> lock(file_lock);
  u64 base = 0;
  switch (origin) {
    case SeekOrigin::kBegin:
      break;
    case SeekOrigin::kCurrent:
      base = position_;
      break;
    case SeekOrigin::kEnd:
      base = file->GetInfo().size;
      break;
  }
  position_ = base + static_cast<u64>(distance);
  return position_;
}

X_STATUS FileObject::SetLengthToPosition() {
  std::lock_guard<std::mutex> lock(file_lock);
  auto status = file->SetLength(position_);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_IMPLEMENTED);
}

u64 FileObject::GetPosition() {
  std::lock_guard<std::mutex> lock(file_lock);
  return position_;
}

void FileObject::SetPosition(u64 value) {
  std::lock_guard<std::mutex> lock(file_lock);
  position_ = value;
}

void FileObject::SetFindPattern(std::string_view pattern) {
  auto listing = file->List(pattern);
  find = listing ? std::move(*listing) : nullptr;
}

std::optional<filesystem::FileStat> FileObject::FindNext() {
  if (!find) {
    return std::nullopt;
  }
  return find->Next();
}

void FileObject::RegisterIOCompletionPort(TitleRuntime& runtime, u32 key, X_HANDLE port) {
  IObjectTable* table = &runtime.GetObjectTable();
  Result<X_HANDLE> owned = table->Duplicate(port);
  if (!owned) {
    return;
  }

  std::lock_guard<std::mutex> lock(completion_port_lock);
  port_table = table;
  completion_ports.emplace_back(key, *owned);
}

}  // namespace rex::kernel
