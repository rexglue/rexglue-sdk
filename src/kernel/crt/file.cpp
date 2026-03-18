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
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xtypes.h>

using rex::X_STATUS;
using namespace rex::ppc;

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

ppc_u32_result_t CreateFileA_entry(ppc_pchar_t lpFileName, ppc_u32_t dwDesiredAccess,
                                   ppc_u32_t dwShareMode, ppc_pvoid_t lpSecurityAttributes,
                                   ppc_u32_t dwCreationDisposition, ppc_u32_t dwFlagsAndAttributes,
                                   ppc_u32_t hTemplateFile) {
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

ppc_u32_result_t ReadFile_entry(ppc_u32_t hFile, ppc_pvoid_t lpBuffer,
                                ppc_u32_t nNumberOfBytesToRead, ppc_pu32_t lpNumberOfBytesRead,
                                ppc_pvoid_t lpOverlapped) {
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

ppc_u32_result_t WriteFile_entry(ppc_u32_t hFile, ppc_pvoid_t lpBuffer,
                                 ppc_u32_t nNumberOfBytesToWrite, ppc_pu32_t lpNumberOfBytesWritten,
                                 ppc_pvoid_t lpOverlapped) {
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

ppc_u32_result_t SetFilePointer_entry(ppc_u32_t hFile, ppc_u32_t lDistanceToMove,
                                      ppc_pu32_t lpDistanceToMoveHigh, ppc_u32_t dwMoveMethod) {
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

ppc_u32_result_t GetFileSize_entry(ppc_u32_t hFile, ppc_pu32_t lpFileSizeHigh) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return kInvalidHandleValue;

  uint64_t size = file->entry()->size();
  if (lpFileSizeHigh)
    *lpFileSizeHigh = static_cast<uint32_t>(size >> 32);
  return static_cast<uint32_t>(size & 0xFFFFFFFF);
}

ppc_u32_result_t GetFileSizeEx_entry(ppc_u32_t hFile, ppc_pvoid_t lpFileSize) {
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

ppc_u32_result_t SetEndOfFile_entry(ppc_u32_t hFile) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return 0;
  X_STATUS status = file->SetLength(file->position());
  return XSUCCEEDED(status) ? 1u : 0u;
}

ppc_u32_result_t FlushFileBuffers_entry(ppc_u32_t hFile) {
  (void)hFile;
  return 1;
}

ppc_u32_result_t DeleteFileA_entry(ppc_pchar_t lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  bool ok = REX_KERNEL_FS()->DeletePath(path);
  if (!ok)
    REXKRNL_DEBUG("rexcrt_DeleteFileA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

ppc_u32_result_t CloseHandle_entry(ppc_u32_t hObject) {
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

static void FillFindData(ppc_pvoid_t lpFindFileData, rex::filesystem::Entry* entry) {
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

ppc_u32_result_t FindFirstFileA_entry(ppc_pchar_t lpFileName, ppc_pvoid_t lpFindFileData) {
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

ppc_u32_result_t FindNextFileA_entry(ppc_u32_t hFindFile, ppc_pvoid_t lpFindFileData) {
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

ppc_u32_result_t FindClose_entry(ppc_u32_t hFindFile) {
  return CloseHandle_entry(hFindFile);
}

ppc_u32_result_t CreateDirectoryA_entry(ppc_pchar_t lpPathName, ppc_pvoid_t lpSecurityAttributes) {
  const char* path = static_cast<const char*>(lpPathName);
  auto* entry = REX_KERNEL_FS()->CreatePath(path, rex::filesystem::kFileAttributeDirectory);
  return entry ? 1u : 0u;
}

ppc_u32_result_t MoveFileA_entry(ppc_pchar_t lpExistingFileName, ppc_pchar_t lpNewFileName) {
  REXKRNL_WARN("rexcrt_MoveFileA: STUB '{}' -> '{}'", static_cast<const char*>(lpExistingFileName),
               static_cast<const char*>(lpNewFileName));
  return 1;
}

ppc_u32_result_t SetFileAttributesA_entry(ppc_pchar_t lpFileName, ppc_u32_t dwFileAttributes) {
  (void)lpFileName;
  (void)dwFileAttributes;
  return 1;
}

ppc_u32_result_t GetFileAttributesA_entry(ppc_pchar_t lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  auto* entry = REX_KERNEL_FS()->ResolvePath(path);
  if (!entry) {
    REXKRNL_DEBUG("rexcrt_GetFileAttributesA: not found '{}'", path);
    return kInvalidHandleValue;  // INVALID_FILE_ATTRIBUTES
  }
  REXKRNL_DEBUG("rexcrt_GetFileAttributesA: '{}' -> attrs={:#x}", path, entry->attributes());
  return entry->attributes();
}

ppc_u32_result_t GetFileAttributesExA_entry(ppc_u32_t fInfoLevelId, ppc_pchar_t lpFileName,
                                            ppc_pvoid_t lpFileInformation) {
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

ppc_u32_result_t SetFilePointerEx_entry(ppc_u32_t hFile, ppc_u32_t distHigh, ppc_u32_t distLow,
                                        ppc_pvoid_t lpNewFilePointer, ppc_u32_t dwMoveMethod) {
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

ppc_u32_result_t SetFileTime_entry(ppc_u32_t hFile, ppc_pvoid_t lpCreationTime,
                                   ppc_pvoid_t lpLastAccessTime, ppc_pvoid_t lpLastWriteTime) {
  // VFS doesn't support modifying timestamps; report success.
  (void)hFile;
  (void)lpCreationTime;
  (void)lpLastAccessTime;
  (void)lpLastWriteTime;
  return 1;
}

ppc_u32_result_t CompareFileTime_entry(ppc_pvoid_t lpFileTime1, ppc_pvoid_t lpFileTime2) {
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

ppc_u32_result_t CopyFileA_entry(ppc_pchar_t lpExistingFileName, ppc_pchar_t lpNewFileName,
                                 ppc_u32_t bFailIfExists) {
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

ppc_u32_result_t RemoveDirectoryA_entry(ppc_pchar_t lpPathName) {
  const char* path = static_cast<const char*>(lpPathName);
  bool ok = REX_KERNEL_FS()->DeletePath(path);
  if (!ok)
    REXKRNL_DEBUG("rexcrt_RemoveDirectoryA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

ppc_u32_result_t GetFileType_entry(ppc_u32_t hFile) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return 0;  // FILE_TYPE_UNKNOWN
  return 1;    // FILE_TYPE_DISK
}

ppc_u32_result_t GetDiskFreeSpaceExA_entry(ppc_pchar_t lpDirectoryName,
                                           ppc_pvoid_t lpFreeBytesAvailableToCaller,
                                           ppc_pvoid_t lpTotalNumberOfBytes,
                                           ppc_pvoid_t lpTotalNumberOfFreeBytes) {
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

REXCRT_EXPORT(rexcrt_CreateFileA, rex::kernel::crt::CreateFileA_entry)
REXCRT_EXPORT(rexcrt_ReadFile, rex::kernel::crt::ReadFile_entry)
REXCRT_EXPORT(rexcrt_WriteFile, rex::kernel::crt::WriteFile_entry)
REXCRT_EXPORT(rexcrt_SetFilePointer, rex::kernel::crt::SetFilePointer_entry)
REXCRT_EXPORT(rexcrt_GetFileSize, rex::kernel::crt::GetFileSize_entry)
REXCRT_EXPORT(rexcrt_GetFileSizeEx, rex::kernel::crt::GetFileSizeEx_entry)
REXCRT_EXPORT(rexcrt_SetEndOfFile, rex::kernel::crt::SetEndOfFile_entry)
REXCRT_EXPORT(rexcrt_FlushFileBuffers, rex::kernel::crt::FlushFileBuffers_entry)
REXCRT_EXPORT(rexcrt_DeleteFileA, rex::kernel::crt::DeleteFileA_entry)
REXCRT_EXPORT(rexcrt_CloseHandle, rex::kernel::crt::CloseHandle_entry)
REXCRT_EXPORT(rexcrt_FindFirstFileA, rex::kernel::crt::FindFirstFileA_entry)
REXCRT_EXPORT(rexcrt_FindNextFileA, rex::kernel::crt::FindNextFileA_entry)
REXCRT_EXPORT(rexcrt_FindClose, rex::kernel::crt::FindClose_entry)
REXCRT_EXPORT(rexcrt_CreateDirectoryA, rex::kernel::crt::CreateDirectoryA_entry)
REXCRT_EXPORT(rexcrt_MoveFileA, rex::kernel::crt::MoveFileA_entry)
REXCRT_EXPORT(rexcrt_SetFileAttributesA, rex::kernel::crt::SetFileAttributesA_entry)
REXCRT_EXPORT(rexcrt_GetFileAttributesA, rex::kernel::crt::GetFileAttributesA_entry)
REXCRT_EXPORT(rexcrt_GetFileAttributesExA, rex::kernel::crt::GetFileAttributesExA_entry)
REXCRT_EXPORT(rexcrt_SetFilePointerEx, rex::kernel::crt::SetFilePointerEx_entry)
REXCRT_EXPORT(rexcrt_SetFileTime, rex::kernel::crt::SetFileTime_entry)
REXCRT_EXPORT(rexcrt_CompareFileTime, rex::kernel::crt::CompareFileTime_entry)
REXCRT_EXPORT(rexcrt_CopyFileA, rex::kernel::crt::CopyFileA_entry)
REXCRT_EXPORT(rexcrt_RemoveDirectoryA, rex::kernel::crt::RemoveDirectoryA_entry)
REXCRT_EXPORT(rexcrt_GetFileType, rex::kernel::crt::GetFileType_entry)

// XAM exports -- same implementations, for games that import file I/O from xam.xex
XAM_EXPORT(__imp__CreateFileA, rex::kernel::crt::CreateFileA_entry)
XAM_EXPORT(__imp__ReadFile, rex::kernel::crt::ReadFile_entry)
XAM_EXPORT(__imp__WriteFile, rex::kernel::crt::WriteFile_entry)
XAM_EXPORT(__imp__SetFilePointer, rex::kernel::crt::SetFilePointer_entry)
XAM_EXPORT(__imp__GetFileSize, rex::kernel::crt::GetFileSize_entry)
XAM_EXPORT(__imp__GetFileSizeEx, rex::kernel::crt::GetFileSizeEx_entry)
XAM_EXPORT(__imp__SetEndOfFile, rex::kernel::crt::SetEndOfFile_entry)
XAM_EXPORT(__imp__FlushFileBuffers, rex::kernel::crt::FlushFileBuffers_entry)
XAM_EXPORT(__imp__DeleteFileA, rex::kernel::crt::DeleteFileA_entry)
XAM_EXPORT(__imp__CloseHandle, rex::kernel::crt::CloseHandle_entry)
XAM_EXPORT(__imp__FindFirstFileA, rex::kernel::crt::FindFirstFileA_entry)
XAM_EXPORT(__imp__FindNextFileA, rex::kernel::crt::FindNextFileA_entry)
XAM_EXPORT(__imp__CreateDirectoryA, rex::kernel::crt::CreateDirectoryA_entry)
XAM_EXPORT(__imp__MoveFileA, rex::kernel::crt::MoveFileA_entry)
XAM_EXPORT(__imp__SetFileAttributesA, rex::kernel::crt::SetFileAttributesA_entry)
XAM_EXPORT(__imp__GetFileAttributesA, rex::kernel::crt::GetFileAttributesA_entry)
XAM_EXPORT(__imp__GetFileAttributesExA, rex::kernel::crt::GetFileAttributesExA_entry)
XAM_EXPORT(__imp__SetFilePointerEx, rex::kernel::crt::SetFilePointerEx_entry)
XAM_EXPORT(__imp__SetFileTime, rex::kernel::crt::SetFileTime_entry)
XAM_EXPORT(__imp__CompareFileTime, rex::kernel::crt::CompareFileTime_entry)
XAM_EXPORT(__imp__CopyFileA, rex::kernel::crt::CopyFileA_entry)
XAM_EXPORT(__imp__GetDiskFreeSpaceExA, rex::kernel::crt::GetDiskFreeSpaceExA_entry)
