/**
 * @file        kernel/crt/file.cpp
 *
 * @brief       rexcrt File I/O hooks -- Win32-style CRT wrappers backed by VFS.
 *              Generic implementations with no game-specific logic.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <cstring>
#include <memory>
#include <span>

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xtypes.h>

using rex::X_STATUS;

namespace rex::kernel::crt {

constexpr uint32_t kCreateNew = 1;
constexpr uint32_t kCreateAlways = 2;
constexpr uint32_t kOpenExisting = 3;
constexpr uint32_t kOpenAlways = 4;
constexpr uint32_t kTruncateExisting = 5;

constexpr uint32_t kFileBegin = 0;
constexpr uint32_t kFileCurrent = 1;
constexpr uint32_t kFileEnd = 2;

constexpr uint32_t kInvalidHandleValue = 0xFFFFFFFF;

static rex::filesystem::FileDisposition MapDisposition(uint32_t win32_disp) {
  using FD = rex::filesystem::FileDisposition;
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

u32 CreateFileA_entry(mapped_string lpFileName, u32 dwDesiredAccess, u32 dwShareMode,
                      mapped_void lpSecurityAttributes, u32 dwCreationDisposition,
                      u32 dwFlagsAndAttributes, u32 hTemplateFile) {
  const char* path = static_cast<const char*>(lpFileName);
  auto* ks = REX_KERNEL_STATE();
  auto disposition = MapDisposition(static_cast<uint32_t>(dwCreationDisposition));

  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  X_STATUS status = ks->file_system()->OpenFile(nullptr, path, disposition,
                                                static_cast<uint32_t>(dwDesiredAccess), false, true,
                                                &vfs_file, &action);

  if (XFAILED(status) || !vfs_file) {
    REXKRNL_DEBUG("rexcrt_CreateFileA: FAILED path='{}' status={:#x}", path, status);
    return kInvalidHandleValue;
  }

  auto* xfile = new rex::system::XFile(ks, vfs_file, true);
  auto handle = xfile->handle();
  REXKRNL_DEBUG("rexcrt_CreateFileA: '{}' -> handle={:#x}", path, handle);
  return handle;
}

u32 ReadFile_entry(u32 hFile, mapped_void lpBuffer, u32 nNumberOfBytesToRead,
                   mapped_u32 lpNumberOfBytesRead, mapped_void lpOverlapped) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file) {
    REXKRNL_WARN("rexcrt_ReadFile: invalid handle {:#x}", static_cast<uint32_t>(hFile));
    if (lpNumberOfBytesRead)
      *lpNumberOfBytesRead = 0;
    return 0;
  }

  uint64_t offset = static_cast<uint64_t>(-1);
  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpOverlapped)));
    offset =
        (static_cast<uint64_t>(static_cast<uint32_t>(ov[3])) << 32) | static_cast<uint32_t>(ov[2]);
  }

  uint32_t bytes_read = 0;
  X_STATUS status = file->Read(lpBuffer.guest_address(),
                               static_cast<uint32_t>(nNumberOfBytesToRead), offset, &bytes_read, 0);

  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpOverlapped)));
    ov[0] = 0;
    ov[1] = bytes_read;
  } else if (lpNumberOfBytesRead) {
    *lpNumberOfBytesRead = bytes_read;
  }

  return XSUCCEEDED(status) ? 1u : 0u;
}

u32 WriteFile_entry(u32 hFile, mapped_void lpBuffer, u32 nNumberOfBytesToWrite,
                    mapped_u32 lpNumberOfBytesWritten, mapped_void lpOverlapped) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file) {
    if (lpNumberOfBytesWritten)
      *lpNumberOfBytesWritten = 0;
    return 0;
  }

  uint64_t offset = static_cast<uint64_t>(-1);
  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpOverlapped)));
    offset =
        (static_cast<uint64_t>(static_cast<uint32_t>(ov[3])) << 32) | static_cast<uint32_t>(ov[2]);
  }

  uint32_t bytes_written = 0;
  X_STATUS status =
      file->Write(lpBuffer.guest_address(), static_cast<uint32_t>(nNumberOfBytesToWrite), offset,
                  &bytes_written, 0);

  if (lpOverlapped) {
    auto* ov = reinterpret_cast<rex::be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpOverlapped)));
    ov[0] = 0;
    ov[1] = bytes_written;
  } else if (lpNumberOfBytesWritten) {
    *lpNumberOfBytesWritten = bytes_written;
  }

  return XSUCCEEDED(status) ? 1u : 0u;
}

u32 SetFilePointer_entry(u32 hFile, u32 lDistanceToMove, mapped_u32 lpDistanceToMoveHigh,
                         u32 dwMoveMethod) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return kInvalidHandleValue;

  int64_t distance = static_cast<int32_t>(static_cast<uint32_t>(lDistanceToMove));
  if (lpDistanceToMoveHigh) {
    distance |=
        static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(*lpDistanceToMoveHigh)))
        << 32;
  }

  uint64_t new_pos = 0;
  switch (static_cast<uint32_t>(dwMoveMethod)) {
    case kFileBegin:
      new_pos = static_cast<uint64_t>(distance);
      break;
    case kFileCurrent:
      new_pos = file->position() + distance;
      break;
    case kFileEnd:
      new_pos = file->entry()->size() + distance;
      break;
    default:
      return kInvalidHandleValue;
  }

  file->set_position(new_pos);
  if (lpDistanceToMoveHigh)
    *lpDistanceToMoveHigh = static_cast<uint32_t>(new_pos >> 32);
  return static_cast<uint32_t>(new_pos & 0xFFFFFFFF);
}

u32 GetFileSize_entry(u32 hFile, mapped_u32 lpFileSizeHigh) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return kInvalidHandleValue;

  uint64_t size = file->entry()->size();
  if (lpFileSizeHigh)
    *lpFileSizeHigh = static_cast<uint32_t>(size >> 32);
  return static_cast<uint32_t>(size & 0xFFFFFFFF);
}

u32 GetFileSizeEx_entry(u32 hFile, mapped_void lpFileSize) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return 0;

  uint64_t size = file->entry()->size();
  if (lpFileSize) {
    auto* out =
        reinterpret_cast<rex::be<uint32_t>*>(static_cast<uint8_t*>(static_cast<void*>(lpFileSize)));
    out[0] = static_cast<uint32_t>(size >> 32);
    out[1] = static_cast<uint32_t>(size & 0xFFFFFFFF);
  }
  return 1;
}

u32 SetEndOfFile_entry(u32 hFile) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return 0;
  X_STATUS status = file->SetLength(file->position());
  return XSUCCEEDED(status) ? 1u : 0u;
}

u32 FlushFileBuffers_entry(u32 hFile) {
  (void)hFile;
  return 1;
}

u32 DeleteFileA_entry(mapped_string lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  bool ok = REX_KERNEL_FS()->DeletePath(path);
  if (!ok)
    REXKRNL_DEBUG("rexcrt_DeleteFileA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

u32 CloseHandle_entry(u32 hObject) {
  uint32_t h = static_cast<uint32_t>(hObject);
  if (h == kInvalidHandleValue || h == 0)
    return 0;
  auto status = REX_KERNEL_OBJECTS()->ReleaseHandle(h);
  if (XFAILED(status)) {
    REXKRNL_WARN("rexcrt_CloseHandle: unknown handle {:#x}", h);
    return 0;
  }
  return 1;
}

static void FillFindData(mapped_void lpFindFileData, rex::filesystem::Entry* entry) {
  auto* buf = static_cast<uint8_t*>(static_cast<void*>(lpFindFileData));
  std::memset(buf, 0, 0x140);

  auto* fields = reinterpret_cast<be<uint32_t>*>(buf);
  fields[0] = entry->attributes();  // 0x00 dwFileAttributes
  fields[1] =
      static_cast<uint32_t>(entry->create_timestamp() & 0xFFFFFFFF);   // 0x04 ftCreationTime.Low
  fields[2] = static_cast<uint32_t>(entry->create_timestamp() >> 32);  // 0x08 ftCreationTime.High
  fields[3] =
      static_cast<uint32_t>(entry->access_timestamp() & 0xFFFFFFFF);   // 0x0C ftLastAccessTime.Low
  fields[4] = static_cast<uint32_t>(entry->access_timestamp() >> 32);  // 0x10 ftLastAccessTime.High
  fields[5] =
      static_cast<uint32_t>(entry->write_timestamp() & 0xFFFFFFFF);   // 0x14 ftLastWriteTime.Low
  fields[6] = static_cast<uint32_t>(entry->write_timestamp() >> 32);  // 0x18 ftLastWriteTime.High
  fields[7] = static_cast<uint32_t>(entry->size() >> 32);             // 0x1C nFileSizeHigh
  fields[8] = static_cast<uint32_t>(entry->size() & 0xFFFFFFFF);      // 0x20 nFileSizeLow
  // 0x24 dwReserved0, 0x28 dwReserved1 already zero

  // 0x2C cFileName[260]
  const auto& name = entry->name();
  std::strncpy(reinterpret_cast<char*>(buf + 0x2C), name.c_str(), 259);
  // 0x130 cAlternateFileName[14] already zero
}

u32 FindFirstFileA_entry(mapped_string lpFileName, mapped_void lpFindFileData) {
  const char* path = static_cast<const char*>(lpFileName);
  auto dir = rex::string::utf8_find_base_guest_path(path);
  auto pattern = rex::string::utf8_find_name_from_guest_path(path);

  auto* ks = REX_KERNEL_STATE();
  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  X_STATUS status = ks->file_system()->OpenFile(
      nullptr, dir, rex::filesystem::FileDisposition::kOpen, 0, true, false, &vfs_file, &action);
  if (XFAILED(status) || !vfs_file) {
    REXKRNL_DEBUG("rexcrt_FindFirstFileA: dir not found '{}'", dir);
    return kInvalidHandleValue;
  }

  auto* xfile = new rex::system::XFile(ks, vfs_file, true);
  xfile->SetFindPattern(pattern);

  auto* entry = xfile->FindNext();
  if (!entry) {
    REXKRNL_DEBUG("rexcrt_FindFirstFileA: no matches for '{}' in '{}'", pattern, dir);
    REX_KERNEL_OBJECTS()->ReleaseHandle(xfile->handle());
    return kInvalidHandleValue;
  }

  FillFindData(lpFindFileData, entry);
  REXKRNL_DEBUG("rexcrt_FindFirstFileA: '{}' first match='{}' handle={:#x}", path, entry->name(),
                xfile->handle());
  return xfile->handle();
}

u32 FindNextFileA_entry(u32 hFindFile, mapped_void lpFindFileData) {
  auto file =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFindFile));
  if (!file)
    return 0;

  auto* entry = file->FindNext();
  if (!entry)
    return 0;

  FillFindData(lpFindFileData, entry);
  return 1;
}

u32 FindClose_entry(u32 hFindFile) {
  return CloseHandle_entry(hFindFile);
}

u32 CreateDirectoryA_entry(mapped_string lpPathName, mapped_void lpSecurityAttributes) {
  const char* path = static_cast<const char*>(lpPathName);
  auto* entry = REX_KERNEL_FS()->CreatePath(path, rex::filesystem::kFileAttributeDirectory);
  return entry ? 1u : 0u;
}

u32 MoveFileA_entry(mapped_string lpExistingFileName, mapped_string lpNewFileName) {
  REXKRNL_WARN("rexcrt_MoveFileA: STUB '{}' -> '{}'", static_cast<const char*>(lpExistingFileName),
               static_cast<const char*>(lpNewFileName));
  return 1;
}

u32 SetFileAttributesA_entry(mapped_string lpFileName, u32 dwFileAttributes) {
  (void)lpFileName;
  (void)dwFileAttributes;
  return 1;
}

u32 GetFileAttributesA_entry(mapped_string lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  auto* entry = REX_KERNEL_FS()->ResolvePath(path);
  if (!entry) {
    REXKRNL_DEBUG("rexcrt_GetFileAttributesA: not found '{}'", path);
    return kInvalidHandleValue;  // INVALID_FILE_ATTRIBUTES
  }
  REXKRNL_DEBUG("rexcrt_GetFileAttributesA: '{}' -> attrs={:#x}", path, entry->attributes());
  return entry->attributes();
}

u32 GetFileAttributesExA_entry(u32 fInfoLevelId, mapped_string lpFileName,
                               mapped_void lpFileInformation) {
  const char* path = static_cast<const char*>(lpFileName);
  auto* entry = REX_KERNEL_FS()->ResolvePath(path);
  if (!entry) {
    REXKRNL_DEBUG("rexcrt_GetFileAttributesExA: not found '{}'", path);
    return 0;
  }

  // Fill WIN32_FILE_ATTRIBUTE_DATA (GetFileExInfoStandard = 0)
  auto* buf =
      reinterpret_cast<be<uint32_t>*>(static_cast<uint8_t*>(static_cast<void*>(lpFileInformation)));
  buf[0] = entry->attributes();                                            // dwFileAttributes
  buf[1] = static_cast<uint32_t>(entry->create_timestamp() & 0xFFFFFFFF);  // ftCreationTime.Low
  buf[2] = static_cast<uint32_t>(entry->create_timestamp() >> 32);         // ftCreationTime.High
  buf[3] = static_cast<uint32_t>(entry->access_timestamp() & 0xFFFFFFFF);  // ftLastAccessTime.Low
  buf[4] = static_cast<uint32_t>(entry->access_timestamp() >> 32);         // ftLastAccessTime.High
  buf[5] = static_cast<uint32_t>(entry->write_timestamp() & 0xFFFFFFFF);   // ftLastWriteTime.Low
  buf[6] = static_cast<uint32_t>(entry->write_timestamp() >> 32);          // ftLastWriteTime.High
  buf[7] = static_cast<uint32_t>(entry->size() >> 32);                     // nFileSizeHigh
  buf[8] = static_cast<uint32_t>(entry->size() & 0xFFFFFFFF);              // nFileSizeLow
  return 1;
}

u32 SetFilePointerEx_entry(u32 hFile, u32 distHigh, u32 distLow, mapped_void lpNewFilePointer,
                           u32 dwMoveMethod) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return 0;

  int64_t distance =
      static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(distHigh)) << 32) |
                           static_cast<uint32_t>(distLow));

  uint64_t new_pos = 0;
  switch (static_cast<uint32_t>(dwMoveMethod)) {
    case kFileBegin:
      new_pos = static_cast<uint64_t>(distance);
      break;
    case kFileCurrent:
      new_pos = file->position() + distance;
      break;
    case kFileEnd:
      new_pos = file->entry()->size() + distance;
      break;
    default:
      return 0;
  }

  file->set_position(new_pos);

  if (lpNewFilePointer) {
    auto* out = reinterpret_cast<be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpNewFilePointer)));
    out[0] = static_cast<uint32_t>(new_pos & 0xFFFFFFFF);  // LowPart
    out[1] = static_cast<uint32_t>(new_pos >> 32);         // HighPart
  }
  return 1;
}

u32 SetFileTime_entry(u32 hFile, mapped_void lpCreationTime, mapped_void lpLastAccessTime,
                      mapped_void lpLastWriteTime) {
  // VFS doesn't support modifying timestamps; report success.
  (void)hFile;
  (void)lpCreationTime;
  (void)lpLastAccessTime;
  (void)lpLastWriteTime;
  return 1;
}

u32 CompareFileTime_entry(mapped_void lpFileTime1, mapped_void lpFileTime2) {
  auto* ft1 =
      reinterpret_cast<be<uint32_t>*>(static_cast<uint8_t*>(static_cast<void*>(lpFileTime1)));
  auto* ft2 =
      reinterpret_cast<be<uint32_t>*>(static_cast<uint8_t*>(static_cast<void*>(lpFileTime2)));
  // FILETIME: { dwLowDateTime, dwHighDateTime }
  uint64_t t1 =
      (static_cast<uint64_t>(static_cast<uint32_t>(ft1[1])) << 32) | static_cast<uint32_t>(ft1[0]);
  uint64_t t2 =
      (static_cast<uint64_t>(static_cast<uint32_t>(ft2[1])) << 32) | static_cast<uint32_t>(ft2[0]);
  if (t1 < t2)
    return static_cast<uint32_t>(-1);
  if (t1 > t2)
    return 1u;
  return 0u;
}

u32 CopyFileA_entry(mapped_string lpExistingFileName, mapped_string lpNewFileName,
                    u32 bFailIfExists) {
  const char* src = static_cast<const char*>(lpExistingFileName);
  const char* dst = static_cast<const char*>(lpNewFileName);

  auto* ks = REX_KERNEL_STATE();

  // Open source for reading
  rex::filesystem::File* src_file = nullptr;
  rex::filesystem::FileAction action;
  X_STATUS status = ks->file_system()->OpenFile(
      nullptr, src, rex::filesystem::FileDisposition::kOpen,
      rex::filesystem::FileAccess::kFileReadData, false, true, &src_file, &action);
  if (XFAILED(status) || !src_file) {
    REXKRNL_DEBUG("rexcrt_CopyFileA: failed to open source '{}'", src);
    return 0;
  }

  // Open/create destination
  auto disp = static_cast<uint32_t>(bFailIfExists) ? rex::filesystem::FileDisposition::kCreate
                                                   : rex::filesystem::FileDisposition::kOverwriteIf;
  rex::filesystem::File* dst_file = nullptr;
  status =
      ks->file_system()->OpenFile(nullptr, dst, disp, rex::filesystem::FileAccess::kFileWriteData,
                                  false, true, &dst_file, &action);
  if (XFAILED(status) || !dst_file) {
    src_file->Destroy();
    REXKRNL_DEBUG("rexcrt_CopyFileA: failed to open dest '{}'", dst);
    return 0;
  }

  // Copy data in 64KB chunks
  constexpr size_t kBufSize = 65536;
  auto buf = std::make_unique<uint8_t[]>(kBufSize);
  uint64_t offset = 0;
  bool ok = true;
  for (;;) {
    size_t bytes_read = 0;
    status = src_file->ReadSync(std::span<uint8_t>(buf.get(), kBufSize), offset, &bytes_read);
    if (XFAILED(status) || bytes_read == 0)
      break;

    size_t bytes_written = 0;
    status = dst_file->WriteSync(std::span<const uint8_t>(buf.get(), bytes_read), offset,
                                 &bytes_written);
    if (XFAILED(status) || bytes_written != bytes_read) {
      ok = false;
      break;
    }
    offset += bytes_read;
  }

  dst_file->Destroy();
  src_file->Destroy();
  REXKRNL_DEBUG("rexcrt_CopyFileA: '{}' -> '{}' {}", src, dst, ok ? "OK" : "FAILED");
  return ok ? 1u : 0u;
}

u32 RemoveDirectoryA_entry(mapped_string lpPathName) {
  const char* path = static_cast<const char*>(lpPathName);
  bool ok = REX_KERNEL_FS()->DeletePath(path);
  if (!ok)
    REXKRNL_DEBUG("rexcrt_RemoveDirectoryA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

u32 GetFileType_entry(u32 hFile) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return 0;  // FILE_TYPE_UNKNOWN
  return 1;    // FILE_TYPE_DISK
}

u32 GetDiskFreeSpaceExA_entry(mapped_string lpDirectoryName,
                              mapped_void lpFreeBytesAvailableToCaller,
                              mapped_void lpTotalNumberOfBytes,
                              mapped_void lpTotalNumberOfFreeBytes) {
  const char* path = static_cast<const char*>(lpDirectoryName);
  auto* entry = REX_KERNEL_FS()->ResolvePath(path);
  if (!entry) {
    REXKRNL_DEBUG("rexcrt_GetDiskFreeSpaceExA: path not found '{}'", path);
    return 0;
  }

  auto* dev = entry->device();
  uint64_t bytes_per_au =
      static_cast<uint64_t>(dev->sectors_per_allocation_unit()) * dev->bytes_per_sector();
  uint64_t total_bytes = static_cast<uint64_t>(dev->total_allocation_units()) * bytes_per_au;
  uint64_t free_bytes = static_cast<uint64_t>(dev->available_allocation_units()) * bytes_per_au;

  if (lpFreeBytesAvailableToCaller) {
    auto* out = reinterpret_cast<be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpFreeBytesAvailableToCaller)));
    out[0] = static_cast<uint32_t>(free_bytes & 0xFFFFFFFF);
    out[1] = static_cast<uint32_t>(free_bytes >> 32);
  }
  if (lpTotalNumberOfBytes) {
    auto* out = reinterpret_cast<be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpTotalNumberOfBytes)));
    out[0] = static_cast<uint32_t>(total_bytes & 0xFFFFFFFF);
    out[1] = static_cast<uint32_t>(total_bytes >> 32);
  }
  if (lpTotalNumberOfFreeBytes) {
    auto* out = reinterpret_cast<be<uint32_t>*>(
        static_cast<uint8_t*>(static_cast<void*>(lpTotalNumberOfFreeBytes)));
    out[0] = static_cast<uint32_t>(free_bytes & 0xFFFFFFFF);
    out[1] = static_cast<uint32_t>(free_bytes >> 32);
  }

  REXKRNL_DEBUG("rexcrt_GetDiskFreeSpaceExA: '{}' total={}MB free={}MB", path,
                total_bytes / (1024 * 1024), free_bytes / (1024 * 1024));
  return 1;
}

}  // namespace rex::kernel::crt

REX_HOOK(rexcrt_CreateFileA, rex::kernel::crt::CreateFileA_entry)
REX_HOOK(rexcrt_ReadFile, rex::kernel::crt::ReadFile_entry)
REX_HOOK(rexcrt_WriteFile, rex::kernel::crt::WriteFile_entry)
REX_HOOK(rexcrt_SetFilePointer, rex::kernel::crt::SetFilePointer_entry)
REX_HOOK(rexcrt_GetFileSize, rex::kernel::crt::GetFileSize_entry)
REX_HOOK(rexcrt_GetFileSizeEx, rex::kernel::crt::GetFileSizeEx_entry)
REX_HOOK(rexcrt_SetEndOfFile, rex::kernel::crt::SetEndOfFile_entry)
REX_HOOK(rexcrt_FlushFileBuffers, rex::kernel::crt::FlushFileBuffers_entry)
REX_HOOK(rexcrt_DeleteFileA, rex::kernel::crt::DeleteFileA_entry)
REX_HOOK(rexcrt_CloseHandle, rex::kernel::crt::CloseHandle_entry)
REX_HOOK(rexcrt_FindFirstFileA, rex::kernel::crt::FindFirstFileA_entry)
REX_HOOK(rexcrt_FindNextFileA, rex::kernel::crt::FindNextFileA_entry)
REX_HOOK(rexcrt_FindClose, rex::kernel::crt::FindClose_entry)
REX_HOOK(rexcrt_CreateDirectoryA, rex::kernel::crt::CreateDirectoryA_entry)
REX_HOOK(rexcrt_MoveFileA, rex::kernel::crt::MoveFileA_entry)
REX_HOOK(rexcrt_SetFileAttributesA, rex::kernel::crt::SetFileAttributesA_entry)
REX_HOOK(rexcrt_GetFileAttributesA, rex::kernel::crt::GetFileAttributesA_entry)
REX_HOOK(rexcrt_GetFileAttributesExA, rex::kernel::crt::GetFileAttributesExA_entry)
REX_HOOK(rexcrt_SetFilePointerEx, rex::kernel::crt::SetFilePointerEx_entry)
REX_HOOK(rexcrt_SetFileTime, rex::kernel::crt::SetFileTime_entry)
REX_HOOK(rexcrt_CompareFileTime, rex::kernel::crt::CompareFileTime_entry)
REX_HOOK(rexcrt_CopyFileA, rex::kernel::crt::CopyFileA_entry)
REX_HOOK(rexcrt_RemoveDirectoryA, rex::kernel::crt::RemoveDirectoryA_entry)
REX_HOOK(rexcrt_GetFileType, rex::kernel::crt::GetFileType_entry)

// XAM exports -- same implementations, for games that import file I/O from xam.xex
REX_EXPORT(__imp__CreateFileA, rex::kernel::crt::CreateFileA_entry)
REX_EXPORT(__imp__ReadFile, rex::kernel::crt::ReadFile_entry)
REX_EXPORT(__imp__WriteFile, rex::kernel::crt::WriteFile_entry)
REX_EXPORT(__imp__SetFilePointer, rex::kernel::crt::SetFilePointer_entry)
REX_EXPORT(__imp__GetFileSize, rex::kernel::crt::GetFileSize_entry)
REX_EXPORT(__imp__GetFileSizeEx, rex::kernel::crt::GetFileSizeEx_entry)
REX_EXPORT(__imp__SetEndOfFile, rex::kernel::crt::SetEndOfFile_entry)
REX_EXPORT(__imp__FlushFileBuffers, rex::kernel::crt::FlushFileBuffers_entry)
REX_EXPORT(__imp__DeleteFileA, rex::kernel::crt::DeleteFileA_entry)
REX_EXPORT(__imp__CloseHandle, rex::kernel::crt::CloseHandle_entry)
REX_EXPORT(__imp__FindFirstFileA, rex::kernel::crt::FindFirstFileA_entry)
REX_EXPORT(__imp__FindNextFileA, rex::kernel::crt::FindNextFileA_entry)
REX_EXPORT(__imp__CreateDirectoryA, rex::kernel::crt::CreateDirectoryA_entry)
REX_EXPORT(__imp__MoveFileA, rex::kernel::crt::MoveFileA_entry)
REX_EXPORT(__imp__SetFileAttributesA, rex::kernel::crt::SetFileAttributesA_entry)
REX_EXPORT(__imp__GetFileAttributesA, rex::kernel::crt::GetFileAttributesA_entry)
REX_EXPORT(__imp__GetFileAttributesExA, rex::kernel::crt::GetFileAttributesExA_entry)
REX_EXPORT(__imp__SetFilePointerEx, rex::kernel::crt::SetFilePointerEx_entry)
REX_EXPORT(__imp__SetFileTime, rex::kernel::crt::SetFileTime_entry)
REX_EXPORT(__imp__CompareFileTime, rex::kernel::crt::CompareFileTime_entry)
REX_EXPORT(__imp__CopyFileA, rex::kernel::crt::CopyFileA_entry)
REX_EXPORT(__imp__GetDiskFreeSpaceExA, rex::kernel::crt::GetDiskFreeSpaceExA_entry)
