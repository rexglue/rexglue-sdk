/**
 * @file        kernel/xam/xam_file.cpp
 * @brief       File routines served to the Xam import table and to the shim layer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstring>
#include <memory>
#include <span>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/hook.h>
#include <rex/kernel/shim/define.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xtypes.h>

using rex::X_STATUS;

namespace rex::kernel::xam {

constexpr u32 kCreateNew = 1;
constexpr u32 kCreateAlways = 2;
constexpr u32 kOpenExisting = 3;
constexpr u32 kOpenAlways = 4;
constexpr u32 kTruncateExisting = 5;

constexpr u32 kFileBegin = 0;
constexpr u32 kFileCurrent = 1;
constexpr u32 kFileEnd = 2;

constexpr u32 kInvalidHandleValue = 0xFFFFFFFF;

static filesystem::Disposition MapDisposition(u32 win32_disp) {
  using FD = filesystem::Disposition;
  switch (win32_disp) {
    case kCreateNew:
      return FD::kCreate;
    case kCreateAlways:
      return FD::kOverwriteIf;
    case kOpenExisting:
      return FD::kOpen;
    case kOpenAlways:
      return FD::kOpenIf;
    case kTruncateExisting:
      return FD::kOverwrite;
    default:
      return FD::kOpen;
  }
}

REX_SHIM_FN(CreateFileA, u32, mapped_string lpFileName, u32 dwDesiredAccess, u32 dwShareMode,
            mapped_void lpSecurityAttributes, u32 dwCreationDisposition, u32 dwFlagsAndAttributes,
            u32 hTemplateFile) {
  const char* path = static_cast<const char*>(lpFileName);
  auto* ks = REX_KERNEL_STATE();
  auto disposition = MapDisposition(static_cast<u32>(dwCreationDisposition));

  filesystem::OpenOptions options;
  options.disposition = disposition;
  options.access = static_cast<filesystem::Access>(static_cast<u32>(dwDesiredAccess));
  options.require_file = true;
  auto opened = ks->file_system()->Open(path, options);
  if (!opened) {
    REXLOG_DEV(kernel, "rexshim_CreateFileA: FAILED path='{}' status={:#x}", path,
               rex::system::ToXStatus(opened.error()));
    return kInvalidHandleValue;
  }

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XFAILED(ks->object_table()->Emplace<rex::system::FileObject>(
          &handle, rex::system::AddOptions{}, std::move(opened->file), true))) {
    return kInvalidHandleValue;
  }
  REXLOG_DEV(kernel, "rexshim_CreateFileA: '{}' -> handle={:#x}", path, handle);
  return handle;
}

REX_SHIM_FN(ReadFile, u32, u32 hFile, mapped_void lpBuffer, u32 nNumberOfBytesToRead,
            mapped_u32 lpNumberOfBytesRead, mapped_void lpOverlapped) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file) {
    REXLOG_WARN(kernel, "rexshim_ReadFile: invalid handle {:#x}", static_cast<u32>(hFile));
    if (lpNumberOfBytesRead)
      *lpNumberOfBytesRead = 0;
    return 0;
  }

  u64 offset = static_cast<u64>(-1);
  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<u32>*>(static_cast<u8*>(static_cast<void*>(lpOverlapped)));
    offset = (static_cast<u64>(static_cast<u32>(ov[3])) << 32) | static_cast<u32>(ov[2]);
  }

  u32 bytes_read = 0;
  X_STATUS status =
      rex::system::FileRead(REX_KERNEL_STATE(), *file, lpBuffer.guest_address(),
                            static_cast<u32>(nNumberOfBytesToRead), offset, &bytes_read, 0);

  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<u32>*>(static_cast<u8*>(static_cast<void*>(lpOverlapped)));
    ov[0] = 0;
    ov[1] = bytes_read;
  } else if (lpNumberOfBytesRead) {
    *lpNumberOfBytesRead = bytes_read;
  }

  return XSUCCEEDED(status) ? 1u : 0u;
}

REX_SHIM_FN(WriteFile, u32, u32 hFile, mapped_void lpBuffer, u32 nNumberOfBytesToWrite,
            mapped_u32 lpNumberOfBytesWritten, mapped_void lpOverlapped) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file) {
    if (lpNumberOfBytesWritten)
      *lpNumberOfBytesWritten = 0;
    return 0;
  }

  u64 offset = static_cast<u64>(-1);
  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<u32>*>(static_cast<u8*>(static_cast<void*>(lpOverlapped)));
    offset = (static_cast<u64>(static_cast<u32>(ov[3])) << 32) | static_cast<u32>(ov[2]);
  }

  u32 bytes_written = 0;
  X_STATUS status =
      rex::system::FileWrite(REX_KERNEL_STATE(), *file, lpBuffer.guest_address(),
                             static_cast<u32>(nNumberOfBytesToWrite), offset, &bytes_written, 0);

  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<u32>*>(static_cast<u8*>(static_cast<void*>(lpOverlapped)));
    ov[0] = 0;
    ov[1] = bytes_written;
  } else if (lpNumberOfBytesWritten) {
    *lpNumberOfBytesWritten = bytes_written;
  }

  return XSUCCEEDED(status) ? 1u : 0u;
}

REX_SHIM_FN(SetFilePointer, u32, u32 hFile, u32 lDistanceToMove, mapped_u32 lpDistanceToMoveHigh,
            u32 dwMoveMethod) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file)
    return kInvalidHandleValue;

  i64 distance = static_cast<i32>(static_cast<u32>(lDistanceToMove));
  if (lpDistanceToMoveHigh) {
    distance |= static_cast<i64>(static_cast<i32>(static_cast<u32>(*lpDistanceToMoveHigh))) << 32;
  }

  u64 new_pos = 0;
  switch (static_cast<u32>(dwMoveMethod)) {
    case kFileBegin:
      new_pos = static_cast<u64>(distance);
      break;
    case kFileCurrent:
      new_pos = rex::system::FilePosition(*file) + distance;
      break;
    case kFileEnd:
      new_pos = file->file->Info().size + distance;
      break;
    default:
      return kInvalidHandleValue;
  }

  rex::system::FileSetPosition(*file, new_pos);
  if (lpDistanceToMoveHigh)
    *lpDistanceToMoveHigh = static_cast<u32>(new_pos >> 32);
  return static_cast<u32>(new_pos & 0xFFFFFFFF);
}

REX_SHIM_FN(GetFileSize, u32, u32 hFile, mapped_u32 lpFileSizeHigh) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file)
    return kInvalidHandleValue;

  u64 size = file->file->Info().size;
  if (lpFileSizeHigh)
    *lpFileSizeHigh = static_cast<u32>(size >> 32);
  return static_cast<u32>(size & 0xFFFFFFFF);
}

REX_SHIM_FN(GetFileSizeEx, u32, u32 hFile, mapped_void lpFileSize) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file)
    return 0;

  u64 size = file->file->Info().size;
  if (lpFileSize) {
    auto* out = reinterpret_cast<rex::be<u32>*>(static_cast<u8*>(static_cast<void*>(lpFileSize)));
    out[0] = static_cast<u32>(size >> 32);
    out[1] = static_cast<u32>(size & 0xFFFFFFFF);
  }
  return 1;
}

REX_SHIM_FN(SetEndOfFile, u32, u32 hFile) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file)
    return 0;
  X_STATUS status = rex::system::FileSetLength(*file, rex::system::FilePosition(*file));
  return XSUCCEEDED(status) ? 1u : 0u;
}

REX_SHIM_FN(FlushFileBuffers, u32, u32 hFile) {
  (void)hFile;
  return 1;
}

REX_SHIM_FN(DeleteFileA, u32, mapped_string lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  bool ok = REX_KERNEL_FS()->Remove(path).has_value();
  if (!ok)
    REXLOG_DEV(kernel, "rexshim_DeleteFileA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

REX_SHIM_FN(CloseHandle, u32, u32 hObject) {
  u32 h = static_cast<u32>(hObject);
  if (h == kInvalidHandleValue || h == 0)
    return 0;
  auto status = REX_KERNEL_OBJECTS()->Close(h);
  if (XFAILED(status)) {
    REXLOG_WARN(kernel, "rexshim_CloseHandle: unknown handle {:#x}", h);
    return 0;
  }
  return 1;
}

static void FillFindData(mapped_void lpFindFileData, const filesystem::FileInfo& entry) {
  auto* buf = static_cast<u8*>(static_cast<void*>(lpFindFileData));
  std::memset(buf, 0, 0x140);

  auto* fields = reinterpret_cast<be<u32>*>(buf);
  fields[0] = std::to_underlying(entry.attributes);              // 0x00 dwFileAttributes
  fields[1] = static_cast<u32>(entry.create_time & 0xFFFFFFFF);  // 0x04 ftCreationTime.Low
  fields[2] = static_cast<u32>(entry.create_time >> 32);         // 0x08 ftCreationTime.High
  fields[3] = static_cast<u32>(entry.access_time & 0xFFFFFFFF);  // 0x0C ftLastAccessTime.Low
  fields[4] = static_cast<u32>(entry.access_time >> 32);         // 0x10 ftLastAccessTime.High
  fields[5] = static_cast<u32>(entry.write_time & 0xFFFFFFFF);   // 0x14 ftLastWriteTime.Low
  fields[6] = static_cast<u32>(entry.write_time >> 32);          // 0x18 ftLastWriteTime.High
  fields[7] = static_cast<u32>(entry.size >> 32);                // 0x1C nFileSizeHigh
  fields[8] = static_cast<u32>(entry.size & 0xFFFFFFFF);         // 0x20 nFileSizeLow
  // 0x24 dwReserved0, 0x28 dwReserved1 already zero

  // 0x2C cFileName[260]
  const auto& name = entry.name;
  rex::copy_truncating(reinterpret_cast<char*>(buf + 0x2C), name, 260);
  // 0x130 cAlternateFileName[14] already zero
}

REX_SHIM_FN(FindFirstFileA, u32, mapped_string lpFileName, mapped_void lpFindFileData) {
  const char* path = static_cast<const char*>(lpFileName);
  auto dir = rex::utf8_find_base_guest_path(path);
  auto pattern = rex::utf8_find_name_from_guest_path(path);

  auto* ks = REX_KERNEL_STATE();
  filesystem::OpenOptions options;
  options.disposition = filesystem::Disposition::kOpen;
  options.access = filesystem::Access::kNone;
  options.create_directory = true;
  auto opened = ks->file_system()->Open(dir, options);
  if (!opened) {
    REXLOG_DEBUG(kernel, "rexshim_FindFirstFileA: dir not found '{}'", dir);
    return kInvalidHandleValue;
  }

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XFAILED(ks->object_table()->Emplace<rex::system::FileObject>(
          &handle, rex::system::AddOptions{}, std::move(opened->file), true))) {
    return kInvalidHandleValue;
  }
  auto xfile = ks->object_table()->Get<rex::system::FileObject>(handle);
  rex::system::FileSetFindPattern(*xfile, pattern);

  auto entry = rex::system::FileFindNext(*xfile);
  if (!entry) {
    REXLOG_DEBUG(kernel, "rexshim_FindFirstFileA: no matches for '{}' in '{}'", pattern, dir);
    REX_KERNEL_OBJECTS()->Close(handle);
    return kInvalidHandleValue;
  }

  FillFindData(lpFindFileData, *entry);
  REXLOG_TRACE(kernel, "rexshim_FindFirstFileA: '{}' first match='{}' handle={:#x}", path,
               entry->name, handle);
  return handle;
}

REX_SHIM_FN(FindNextFileA, u32, u32 hFindFile, mapped_void lpFindFileData) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFindFile));
  if (!file)
    return 0;

  auto entry = rex::system::FileFindNext(*file);
  if (!entry)
    return 0;

  FillFindData(lpFindFileData, *entry);
  return 1;
}

REX_SHIM_FN(CreateDirectoryA, u32, mapped_string lpPathName, mapped_void lpSecurityAttributes) {
  const char* path = static_cast<const char*>(lpPathName);
  auto* fs = REX_KERNEL_FS();
  if (fs->Stat(path)) {
    return 0;
  }
  return fs->CreateDirectories(path) ? 1u : 0u;
}

REX_SHIM_FN(GetFileAttributesA, u32, mapped_string lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  auto entry = REX_KERNEL_FS()->Stat(path);
  if (!entry) {
    REXLOG_DEBUG(kernel, "rexshim_GetFileAttributesA: not found '{}'", path);
    return kInvalidHandleValue;  // INVALID_FILE_ATTRIBUTES
  }
  REXLOG_TRACE(kernel, "rexshim_GetFileAttributesA: '{}' -> attrs={:#x}", path,
               std::to_underlying(entry->attributes));
  return std::to_underlying(entry->attributes);
}

REX_SHIM_FN(GetFileAttributesExA, u32, u32 fInfoLevelId, mapped_string lpFileName,
            mapped_void lpFileInformation) {
  const char* path = static_cast<const char*>(lpFileName);
  auto entry = REX_KERNEL_FS()->Stat(path);
  if (!entry) {
    REXLOG_DEBUG(kernel, "rexshim_GetFileAttributesExA: not found '{}'", path);
    return 0;
  }

  // Fill WIN32_FILE_ATTRIBUTE_DATA (GetFileExInfoStandard = 0)
  auto* buf = reinterpret_cast<be<u32>*>(static_cast<u8*>(static_cast<void*>(lpFileInformation)));
  buf[0] = std::to_underlying(entry->attributes);              // dwFileAttributes
  buf[1] = static_cast<u32>(entry->create_time & 0xFFFFFFFF);  // ftCreationTime.Low
  buf[2] = static_cast<u32>(entry->create_time >> 32);         // ftCreationTime.High
  buf[3] = static_cast<u32>(entry->access_time & 0xFFFFFFFF);  // ftLastAccessTime.Low
  buf[4] = static_cast<u32>(entry->access_time >> 32);         // ftLastAccessTime.High
  buf[5] = static_cast<u32>(entry->write_time & 0xFFFFFFFF);   // ftLastWriteTime.Low
  buf[6] = static_cast<u32>(entry->write_time >> 32);          // ftLastWriteTime.High
  buf[7] = static_cast<u32>(entry->size >> 32);                // nFileSizeHigh
  buf[8] = static_cast<u32>(entry->size & 0xFFFFFFFF);         // nFileSizeLow
  return 1;
}

REX_SHIM_FN(SetFilePointerEx, u32, u32 hFile, u32 distHigh, u32 distLow,
            mapped_void lpNewFilePointer, u32 dwMoveMethod) {
  auto file = REX_KERNEL_OBJECTS()->Get<rex::system::FileObject>(static_cast<u32>(hFile));
  if (!file)
    return 0;

  i64 distance = static_cast<i64>((static_cast<u64>(static_cast<u32>(distHigh)) << 32) |
                                  static_cast<u32>(distLow));

  u64 new_pos = 0;
  switch (static_cast<u32>(dwMoveMethod)) {
    case kFileBegin:
      new_pos = static_cast<u64>(distance);
      break;
    case kFileCurrent:
      new_pos = rex::system::FilePosition(*file) + distance;
      break;
    case kFileEnd:
      new_pos = file->file->Info().size + distance;
      break;
    default:
      return 0;
  }

  rex::system::FileSetPosition(*file, new_pos);

  if (lpNewFilePointer) {
    auto* out = reinterpret_cast<be<u32>*>(static_cast<u8*>(static_cast<void*>(lpNewFilePointer)));
    out[0] = static_cast<u32>(new_pos & 0xFFFFFFFF);  // LowPart
    out[1] = static_cast<u32>(new_pos >> 32);         // HighPart
  }
  return 1;
}

REX_SHIM_FN(SetFileTime, u32, u32 hFile, mapped_void lpCreationTime, mapped_void lpLastAccessTime,
            mapped_void lpLastWriteTime) {
  // VFS doesn't support modifying timestamps; report success.
  (void)hFile;
  (void)lpCreationTime;
  (void)lpLastAccessTime;
  (void)lpLastWriteTime;
  return 1;
}

REX_SHIM_FN(CompareFileTime, u32, mapped_void lpFileTime1, mapped_void lpFileTime2) {
  auto* ft1 = reinterpret_cast<be<u32>*>(static_cast<u8*>(static_cast<void*>(lpFileTime1)));
  auto* ft2 = reinterpret_cast<be<u32>*>(static_cast<u8*>(static_cast<void*>(lpFileTime2)));
  // FILETIME: { dwLowDateTime, dwHighDateTime }
  u64 t1 = (static_cast<u64>(static_cast<u32>(ft1[1])) << 32) | static_cast<u32>(ft1[0]);
  u64 t2 = (static_cast<u64>(static_cast<u32>(ft2[1])) << 32) | static_cast<u32>(ft2[0]);
  if (t1 < t2)
    return static_cast<u32>(-1);
  if (t1 > t2)
    return 1u;
  return 0u;
}

REX_SHIM_FN(CopyFileA, u32, mapped_string lpExistingFileName, mapped_string lpNewFileName,
            u32 bFailIfExists) {
  const char* src = static_cast<const char*>(lpExistingFileName);
  const char* dst = static_cast<const char*>(lpNewFileName);

  auto* ks = REX_KERNEL_STATE();

  filesystem::OpenOptions source_options;
  source_options.disposition = filesystem::Disposition::kOpen;
  source_options.access = filesystem::Access::kReadData;
  source_options.require_file = true;
  auto src_file = ks->file_system()->Open(src, source_options);
  if (!src_file) {
    REXLOG_DEBUG(kernel, "rexshim_CopyFileA: failed to open source '{}'", src);
    return 0;
  }

  filesystem::OpenOptions destination_options;
  destination_options.disposition = static_cast<u32>(bFailIfExists)
                                        ? filesystem::Disposition::kCreate
                                        : filesystem::Disposition::kOverwriteIf;
  destination_options.access = filesystem::Access::kWriteData;
  destination_options.require_file = true;
  auto dst_file = ks->file_system()->Open(dst, destination_options);
  if (!dst_file) {
    REXLOG_DEBUG(kernel, "rexshim_CopyFileA: failed to open dest '{}'", dst);
    return 0;
  }

  // Copy data in 64KB chunks
  constexpr size_t kBufSize = 65536;
  auto buf = std::make_unique<u8[]>(kBufSize);
  u64 offset = 0;
  bool ok = true;
  for (;;) {
    auto bytes_read = src_file->file->Read(std::span<u8>(buf.get(), kBufSize), offset);
    if (!bytes_read || *bytes_read == 0)
      break;

    auto bytes_written = dst_file->file->Write(std::span<const u8>(buf.get(), *bytes_read), offset);
    if (!bytes_written || *bytes_written != *bytes_read) {
      ok = false;
      break;
    }
    offset += *bytes_read;
  }

  REXLOG_TRACE(kernel, "rexshim_CopyFileA: '{}' -> '{}' {}", src, dst, ok ? "OK" : "FAILED");
  return ok ? 1u : 0u;
}

u32 GetDiskFreeSpaceExA_entry(mapped_string lpDirectoryName,
                              mapped_void lpFreeBytesAvailableToCaller,
                              mapped_void lpTotalNumberOfBytes,
                              mapped_void lpTotalNumberOfFreeBytes) {
  const char* path = static_cast<const char*>(lpDirectoryName);
  auto* fs = REX_KERNEL_FS();
  auto volume = fs->Volume(path);
  if (!fs->Stat(path) || !volume) {
    REXLOG_DEBUG(kernel, "rexshim_GetDiskFreeSpaceExA: path not found '{}'", path);
    return 0;
  }

  u64 bytes_per_au =
      static_cast<u64>(volume->sectors_per_allocation_unit) * volume->bytes_per_sector;
  u64 total_bytes = static_cast<u64>(volume->total_allocation_units) * bytes_per_au;
  u64 free_bytes = static_cast<u64>(volume->available_allocation_units) * bytes_per_au;

  if (lpFreeBytesAvailableToCaller) {
    auto* out = reinterpret_cast<be<u32>*>(
        static_cast<u8*>(static_cast<void*>(lpFreeBytesAvailableToCaller)));
    out[0] = static_cast<u32>(free_bytes & 0xFFFFFFFF);
    out[1] = static_cast<u32>(free_bytes >> 32);
  }
  if (lpTotalNumberOfBytes) {
    auto* out =
        reinterpret_cast<be<u32>*>(static_cast<u8*>(static_cast<void*>(lpTotalNumberOfBytes)));
    out[0] = static_cast<u32>(total_bytes & 0xFFFFFFFF);
    out[1] = static_cast<u32>(total_bytes >> 32);
  }
  if (lpTotalNumberOfFreeBytes) {
    auto* out =
        reinterpret_cast<be<u32>*>(static_cast<u8*>(static_cast<void*>(lpTotalNumberOfFreeBytes)));
    out[0] = static_cast<u32>(free_bytes & 0xFFFFFFFF);
    out[1] = static_cast<u32>(free_bytes >> 32);
  }

  REXLOG_TRACE(kernel, "rexshim_GetDiskFreeSpaceExA: '{}' total={}MB free={}MB", path,
               total_bytes / (1024 * 1024), free_bytes / (1024 * 1024));
  return 1;
}

}  // namespace rex::kernel::xam

REX_EXPORT(__imp__CreateFileA, rex::kernel::xam::CreateFileA_shim)
REX_EXPORT(__imp__ReadFile, rex::kernel::xam::ReadFile_shim)
REX_EXPORT(__imp__WriteFile, rex::kernel::xam::WriteFile_shim)
REX_EXPORT(__imp__SetFilePointer, rex::kernel::xam::SetFilePointer_shim)
REX_EXPORT(__imp__GetFileSize, rex::kernel::xam::GetFileSize_shim)
REX_EXPORT(__imp__GetFileSizeEx, rex::kernel::xam::GetFileSizeEx_shim)
REX_EXPORT(__imp__SetEndOfFile, rex::kernel::xam::SetEndOfFile_shim)
REX_EXPORT(__imp__FlushFileBuffers, rex::kernel::xam::FlushFileBuffers_shim)
REX_EXPORT(__imp__DeleteFileA, rex::kernel::xam::DeleteFileA_shim)
REX_EXPORT(__imp__CloseHandle, rex::kernel::xam::CloseHandle_shim)
REX_EXPORT(__imp__FindFirstFileA, rex::kernel::xam::FindFirstFileA_shim)
REX_EXPORT(__imp__FindNextFileA, rex::kernel::xam::FindNextFileA_shim)
REX_EXPORT(__imp__CreateDirectoryA, rex::kernel::xam::CreateDirectoryA_shim)
REX_EXPORT(__imp__GetFileAttributesA, rex::kernel::xam::GetFileAttributesA_shim)
REX_EXPORT(__imp__GetFileAttributesExA, rex::kernel::xam::GetFileAttributesExA_shim)
REX_EXPORT(__imp__SetFilePointerEx, rex::kernel::xam::SetFilePointerEx_shim)
REX_EXPORT(__imp__SetFileTime, rex::kernel::xam::SetFileTime_shim)
REX_EXPORT(__imp__CompareFileTime, rex::kernel::xam::CompareFileTime_shim)
REX_EXPORT(__imp__CopyFileA, rex::kernel::xam::CopyFileA_shim)
REX_EXPORT(__imp__GetDiskFreeSpaceExA, rex::kernel::xam::GetDiskFreeSpaceExA_entry)
