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

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xfile.h>
#include <rex/runtime/xiocompletion.h>

#include <span>

namespace rex::runtime {

namespace {

void NotifyIOCompletionPorts(Runtime& runtime, FileObject& file, IONotification& notification) {
  std::vector<std::pair<u32, X_HANDLE>> ports;
  {
    std::lock_guard<std::mutex> lock(file.completion_port_lock);
    ports = file.completion_ports;
  }

  for (const auto& port : ports) {
    auto pin = runtime.object_table().Get<IOCompletionObject>(port.second);
    if (pin) {
      notification.key_context = port.first;
      IOCompletionQueue(*pin, notification);
    }
  }
}

X_STATUS ReadInternal(Runtime& runtime, FileObject& file, u32 buffer_guest_address,
                      u32 buffer_length, u64 byte_offset, u32* out_bytes_read, u32 apc_context,
                      bool notify_completion) {
  auto* memory = runtime.memory();
  if (byte_offset == static_cast<u64>(-1)) {
    byte_offset = file.position;
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      u32 buffer_guest_high_address = buffer_guest_address + buffer_length - 1;
      rex::runtime::BaseHeap* buffer_start_heap = memory->LookupHeap(buffer_guest_address);
      const rex::runtime::BaseHeap* buffer_end_heap = memory->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == runtime::HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == runtime::HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == runtime::HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        rex::runtime::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == runtime::HeapType::kGuestPhysical
                ? static_cast<rex::runtime::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap && buffer_physical_heap->QueryRangeAccess(
                                        buffer_guest_address, buffer_guest_high_address) !=
                                        memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          auto read = file.file->Read(
              std::span<u8>(buffer_physical_heap ? memory->TranslatePhysical(
                                                       buffer_physical_heap->GetPhysicalAddress(
                                                           buffer_guest_address))
                                                 : memory->TranslateVirtual(buffer_guest_address),
                            buffer_length),
              byte_offset);
          if (read) {
            bytes_read = *read;
          } else {
            result = ToXStatus(read.error());
          }
          if (XSUCCEEDED(result)) {
            if (buffer_physical_heap) {
              buffer_physical_heap->TriggerCallbacks(buffer_guest_address, buffer_length, true,
                                                     true);
            }

            if (byte_offset) {
              file.position = byte_offset;
            }
            file.position += bytes_read;
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

    NotifyIOCompletionPorts(runtime, file, notify);

    file.async_event->Set();
  }

  return result;
}

}  // namespace

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

X_STATUS FileQueryDirectory(Runtime&, FileObject& file, X_FILE_DIRECTORY_INFORMATION* out_info,
                            size_t length, std::string_view file_name, bool restart) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  REX_ASSERT(out_info != nullptr);

  std::optional<filesystem::FileInfo> entry;

  if (!file_name.empty()) {
    REX_ASSERT(rex::utf8_find_any_of(file_name, "\\") == std::string_view::npos);
    FileSetFindPattern(file, file_name);
    entry = FileFindNext(file);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (!file.find) {
      FileSetFindPattern(file, {});
    } else if (restart) {
      file.find->Restart();
    }
    entry = FileFindNext(file);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<u8*>(out_info) + length;
  const std::string& entry_name = entry->name;
  if (reinterpret_cast<u8*>(&out_info->file_name[0]) + entry_name.size() > end) {
    REX_ASSERT_ALWAYS("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<u32>(file.find->position());
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

X_STATUS FileRead(Runtime& runtime, FileObject& file, u32 buffer_guest_address, u32 buffer_length,
                  u64 byte_offset, u32* out_bytes_read, u32 apc_context, bool notify_completion) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  return ReadInternal(runtime, file, buffer_guest_address, buffer_length, byte_offset,
                      out_bytes_read, apc_context, notify_completion);
}

X_STATUS FileReadScatter(Runtime& runtime, FileObject& file, u32 segments_guest_address, u32 length,
                         u64 byte_offset, u32* out_bytes_read, u32 apc_context) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  X_STATUS result = X_STATUS_SUCCESS;

  be_u32* segments =
      reinterpret_cast<be_u32*>(runtime.memory()->TranslateVirtual(segments_guest_address));

  u32 page_size = 4096;

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
        runtime, file, read_buffer, read_length,
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

  NotifyIOCompletionPorts(runtime, file, notify);

  file.async_event->Set();

  return result;
}

X_STATUS FileWrite(Runtime& runtime, FileObject& file, u32 buffer_guest_address, u32 buffer_length,
                   u64 byte_offset, u32* out_bytes_written, u32 apc_context) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  if (byte_offset == static_cast<u64>(-1)) {
    byte_offset = file.position;
  }

  size_t bytes_written = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  auto written = file.file->Write(
      std::span<const u8>(runtime.memory()->TranslateVirtual(buffer_guest_address), buffer_length),
      byte_offset);
  if (written) {
    bytes_written = *written;
  } else {
    result = ToXStatus(written.error());
  }
  if (XSUCCEEDED(result)) {
    file.position += bytes_written;
  }

  IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = static_cast<u32>(bytes_written);
  notify.status = result;

  NotifyIOCompletionPorts(runtime, file, notify);

  if (out_bytes_written) {
    *out_bytes_written = static_cast<u32>(bytes_written);
  }

  file.async_event->Set();
  return result;
}

X_STATUS FileSetLength(FileObject& file, u64 length) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  auto status = file.file->SetLength(length);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_IMPLEMENTED);
}

X_STATUS FileSetAllocation(FileObject& file, u64 size) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  file.file->Refresh();
  if (size >= file.file->Info().size) {
    return X_STATUS_SUCCESS;
  }
  auto status = file.file->SetLength(size);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_IMPLEMENTED);
}

X_STATUS FileRename(filesystem::IFileSystem& file_system, FileObject& file, std::string_view path,
                    bool replace_existing, FileObject* root) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  auto status =
      file_system.Rename(*file.file, path, replace_existing, root ? root->file.get() : nullptr);
  return status ? X_STATUS_SUCCESS : ToXStatus(status.error(), X_STATUS_NOT_SUPPORTED);
}

u64 FilePosition(FileObject& file) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  return file.position;
}

void FileSetPosition(FileObject& file, u64 value) {
  std::lock_guard<std::mutex> lock(file.file_lock);
  file.position = value;
}

void FileSetFindPattern(FileObject& file, std::string_view pattern) {
  auto listing = file.file->List(pattern);
  file.find = listing ? std::move(*listing) : nullptr;
}

std::optional<filesystem::FileInfo> FileFindNext(FileObject& file) {
  if (!file.find) {
    return std::nullopt;
  }
  return file.find->Next();
}

void FileRegisterIOCompletionPort(Runtime& runtime, FileObject& file, u32 key, X_HANDLE port) {
  IObjectTable* table = &runtime.object_table();
  Result<X_HANDLE> owned = table->Duplicate(port);
  if (!owned) {
    return;
  }

  std::lock_guard<std::mutex> lock(file.completion_port_lock);
  file.port_table = table;
  file.completion_ports.emplace_back(key, *owned);
}

}  // namespace rex::runtime
