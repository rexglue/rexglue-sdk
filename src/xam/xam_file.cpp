/**
 * @file        xam/xam_file.cpp
 * @brief       File routines served to the XAM import table and to the shim layer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "xam/xam_state.h"
#include <cstring>
#include <memory>
#include <span>

#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_open.h>
#include <rex/logging/log_category.h>
#include "xam/categories.h"
#include <rex/memory.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/string/text.h>
#include <rex/filesystem/path.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"

using rex::kernel::X_STATUS;

namespace rex::xam {

constexpr u32 kCreateNew = 1;
constexpr u32 kCreateAlways = 2;
constexpr u32 kOpenExisting = 3;
constexpr u32 kOpenAlways = 4;
constexpr u32 kTruncateExisting = 5;

constexpr u32 kFileEnd = 2;

/** Stores a time as a guest FILETIME, which is the high word then the low word. */
static void StoreFileTime(rex::BE<u32>* out, u64 time) {
  out[0] = static_cast<u32>(time >> 32);
  out[1] = static_cast<u32>(time & 0xFFFFFFFF);
}

static u64 LoadFileTime(const rex::BE<u32>* in) {
  return (static_cast<u64>(static_cast<u32>(in[0])) << 32) | static_cast<u32>(in[1]);
}

/** Sets the last error of the calling thread from an NT status, as RtlSetLastNTError does. */
static void SetLastNTError(X_STATUS status) {
  CurrentTitle().threads.GetCallingThread()->SetLastError(
      kernel::xboxkrnl::RtlNtStatusToDosError(status));
}

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

REX_XAM_SHIM_FN(CreateFileA, u32, MappedString lpFileName, u32 dwDesiredAccess,
                [[maybe_unused]] u32 dwShareMode, [[maybe_unused]] MappedVoid lpSecurityAttributes,
                u32 dwCreationDisposition, [[maybe_unused]] u32 dwFlagsAndAttributes,
                [[maybe_unused]] u32 hTemplateFile) {
  const char* path = static_cast<const char*>(lpFileName);
  auto disposition = MapDisposition(dwCreationDisposition);

  filesystem::OpenOptions options;
  options.disposition = disposition;
  options.access = static_cast<filesystem::Access>(dwDesiredAccess);
  options.require_file = true;
  auto opened = CurrentTitle().file_system.Open(path, options);
  if (!opened) {
    REXXAM_DEV("rexshim_CreateFileA: FAILED path='{}' status={:#x}", path,
               rex::kernel::ToXStatus(opened.error()));
    return kernel::kXInvalidHandleValue;
  }

  auto added = CurrentTitle().objects.AddFile(std::move(opened->file), true);
  if (!added) {
    return kernel::kXInvalidHandleValue;
  }
  const kernel::X_HANDLE handle = *added;
  REXXAM_DEV("rexshim_CreateFileA: '{}' -> handle={:#x}", path, handle);
  return handle;
}

REX_XAM_SHIM_FN(ReadFile, u32, u32 hFile, MappedVoid lpBuffer, u32 nNumberOfBytesToRead,
                MappedU32 lpNumberOfBytesRead, MappedVoid lpOverlapped) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file) {
    REXXAM_WARN("rexshim_ReadFile: invalid handle {:#x}", static_cast<u32>(hFile));
    if (lpNumberOfBytesRead)
      *lpNumberOfBytesRead = 0;
    return 0;
  }

  u64 offset = static_cast<u64>(-1);
  if (lpOverlapped) {
    auto* ov = lpOverlapped.AsArray<u32>();
    offset = (static_cast<u64>(static_cast<u32>(ov[3])) << 32) | static_cast<u32>(ov[2]);
  }

  u32 bytes_read = 0;
  X_STATUS status =
      file->Read(lpBuffer.GetGuestAddress(), nNumberOfBytesToRead, offset, &bytes_read, 0);

  if (lpOverlapped) {
    auto* ov = lpOverlapped.AsArray<u32>();
    ov[0] = 0;
    ov[1] = bytes_read;
  } else if (lpNumberOfBytesRead) {
    *lpNumberOfBytesRead = bytes_read;
  }

  return kernel::XSUCCEEDED(status) ? 1u : 0u;
}

REX_XAM_SHIM_FN(WriteFile, u32, u32 hFile, MappedVoid lpBuffer, u32 nNumberOfBytesToWrite,
                MappedU32 lpNumberOfBytesWritten, MappedVoid lpOverlapped) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file) {
    if (lpNumberOfBytesWritten)
      *lpNumberOfBytesWritten = 0;
    return 0;
  }

  u64 offset = static_cast<u64>(-1);
  if (lpOverlapped) {
    auto* ov = lpOverlapped.AsArray<u32>();
    offset = (static_cast<u64>(static_cast<u32>(ov[3])) << 32) | static_cast<u32>(ov[2]);
  }

  u32 bytes_written = 0;
  X_STATUS status =
      file->Write(lpBuffer.GetGuestAddress(), nNumberOfBytesToWrite, offset, &bytes_written, 0);

  if (lpOverlapped) {
    auto* ov = lpOverlapped.AsArray<u32>();
    ov[0] = 0;
    ov[1] = bytes_written;
  } else if (lpNumberOfBytesWritten) {
    *lpNumberOfBytesWritten = bytes_written;
  }

  return kernel::XSUCCEEDED(status) ? 1u : 0u;
}

REX_XAM_SHIM_FN(SetFilePointer, u32, u32 hFile, u32 lDistanceToMove, MappedU32 lpDistanceToMoveHigh,
                u32 dwMoveMethod) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file)
    return kernel::kXInvalidHandleValue;

  i64 distance = static_cast<i32>(lDistanceToMove);
  if (lpDistanceToMoveHigh) {
    distance |= static_cast<i64>(static_cast<i32>(static_cast<u32>(*lpDistanceToMoveHigh))) << 32;
  }

  if (dwMoveMethod > kFileEnd)
    return kernel::kXInvalidHandleValue;
  const u64 new_pos = file->Seek(distance, static_cast<rex::kernel::SeekOrigin>(dwMoveMethod));
  if (lpDistanceToMoveHigh)
    *lpDistanceToMoveHigh = static_cast<u32>(new_pos >> 32);
  return static_cast<u32>(new_pos & 0xFFFFFFFF);
}

REX_XAM_SHIM_FN(GetFileSize, u32, u32 hFile, MappedU32 lpFileSizeHigh) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file)
    return kernel::kXInvalidHandleValue;

  u64 size = file->GetSize();
  if (lpFileSizeHigh)
    *lpFileSizeHigh = static_cast<u32>(size >> 32);
  return static_cast<u32>(size & 0xFFFFFFFF);
}

REX_XAM_SHIM_FN(GetFileSizeEx, u32, u32 hFile, MappedVoid lpFileSize) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file)
    return 0;

  u64 size = file->GetSize();
  if (lpFileSize) {
    auto* out = lpFileSize.AsArray<u32>();
    out[0] = static_cast<u32>(size >> 32);
    out[1] = static_cast<u32>(size & 0xFFFFFFFF);
  }
  return 1;
}

REX_XAM_SHIM_FN(SetEndOfFile, u32, u32 hFile) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file)
    return 0;
  X_STATUS status = file->SetLengthToPosition();
  return kernel::XSUCCEEDED(status) ? 1u : 0u;
}

REX_XAM_SHIM_FN(FlushFileBuffers, u32, u32 hFile) {
  (void)hFile;
  return 1;
}

REX_XAM_SHIM_FN(DeleteFileA, u32, MappedString lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  bool ok = CurrentTitle().file_system.Remove(path).has_value();
  if (!ok)
    REXXAM_DEV("rexshim_DeleteFileA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

REX_XAM_SHIM_FN(CloseHandle, u32, u32 hObject) {
  u32 h = hObject;
  if (h == kernel::kXInvalidHandleValue || h == 0)
    return 0;
  if (!CurrentTitle().objects.Close(h)) {
    REXXAM_WARN("rexshim_CloseHandle: unknown handle {:#x}", h);
    return 0;
  }
  return 1;
}

/** Fills a WIN32_FIND_DATA of 0x140 bytes. */
static void FillFindData(MappedVoid lpFindFileData, const filesystem::FileStat& entry) {
  auto* buf = lpFindFileData.As<u8*>();
  std::memset(buf, 0, 0x140);

  auto* fields = lpFindFileData.AsArray<u32>();
  fields[0] = std::to_underlying(entry.attributes);
  StoreFileTime(&fields[1], entry.create_time);
  StoreFileTime(&fields[3], entry.access_time);
  StoreFileTime(&fields[5], entry.write_time);
  fields[7] = static_cast<u32>(entry.size >> 32);
  fields[8] = static_cast<u32>(entry.size & 0xFFFFFFFF);

  const auto& name = entry.name;
  rex::string::CopyTruncating(reinterpret_cast<char*>(buf + 0x2C), name, 260);
}

REX_XAM_SHIM_FN(FindFirstFileA, u32, MappedString lpFileName, MappedVoid lpFindFileData) {
  const char* path = static_cast<const char*>(lpFileName);
  auto dir = rex::filesystem::utf8_find_base_guest_path(path);
  auto pattern = rex::filesystem::utf8_find_name_from_guest_path(path);

  filesystem::OpenOptions options;
  options.disposition = filesystem::Disposition::kOpen;
  options.access = filesystem::Access::kNone;
  options.create_directory = true;
  auto opened = CurrentTitle().file_system.Open(dir, options);
  if (!opened) {
    REXXAM_DEBUG("rexshim_FindFirstFileA: dir not found '{}'", dir);
    return kernel::kXInvalidHandleValue;
  }

  auto added = CurrentTitle().objects.AddFile(std::move(opened->file), true);
  if (!added) {
    return kernel::kXInvalidHandleValue;
  }
  const kernel::X_HANDLE handle = *added;
  auto xfile = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(handle);
  xfile->SetFindPattern(pattern);

  auto entry = xfile->FindNext();
  if (!entry) {
    REXXAM_DEBUG("rexshim_FindFirstFileA: no matches for '{}' in '{}'", pattern, dir);
    kernel::CloseOwned(CurrentTitle().objects, handle);
    return kernel::kXInvalidHandleValue;
  }

  FillFindData(lpFindFileData, *entry);
  REXXAM_TRACE("rexshim_FindFirstFileA: '{}' first match='{}' handle={:#x}", path, entry->name,
               handle);
  return handle;
}

REX_XAM_SHIM_FN(FindNextFileA, u32, u32 hFindFile, MappedVoid lpFindFileData) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFindFile);
  if (!file)
    return 0;

  auto entry = file->FindNext();
  if (!entry)
    return 0;

  FillFindData(lpFindFileData, *entry);
  return 1;
}

REX_XAM_SHIM_FN(CreateDirectoryA, u32, MappedString lpPathName,
                [[maybe_unused]] MappedVoid lpSecurityAttributes) {
  const char* path = static_cast<const char*>(lpPathName);
  filesystem::OpenOptions options;
  options.disposition = filesystem::Disposition::kCreate;
  options.access = filesystem::Access::kNone;
  options.create_directory = true;
  auto created = CurrentTitle().file_system.Open(path, options);
  if (!created) {
    SetLastNTError(kernel::ToXStatus(created.error()));
    return 0;
  }
  return 1;
}

REX_XAM_SHIM_FN(GetFileAttributesA, u32, MappedString lpFileName) {
  const char* path = static_cast<const char*>(lpFileName);
  auto entry = CurrentTitle().file_system.Stat(path);
  if (!entry) {
    REXXAM_DEBUG("rexshim_GetFileAttributesA: not found '{}'", path);
    // INVALID_FILE_ATTRIBUTES
    return kernel::kXInvalidHandleValue;
  }
  REXXAM_TRACE("rexshim_GetFileAttributesA: '{}' -> attrs={:#x}", path,
               std::to_underlying(entry->attributes));
  return std::to_underlying(entry->attributes);
}

REX_XAM_SHIM_FN(GetFileAttributesExA, u32, [[maybe_unused]] u32 fInfoLevelId,
                MappedString lpFileName, MappedVoid lpFileInformation) {
  const char* path = static_cast<const char*>(lpFileName);
  auto entry = CurrentTitle().file_system.Stat(path);
  if (!entry) {
    REXXAM_DEBUG("rexshim_GetFileAttributesExA: not found '{}'", path);
    SetLastNTError(kernel::ToXStatus(entry.error()));
    return 0;
  }

  // WIN32_FILE_ATTRIBUTE_DATA for GetFileExInfoStandard (0).
  auto* buf = lpFileInformation.AsArray<u32>();
  buf[0] = std::to_underlying(entry->attributes);
  StoreFileTime(&buf[1], entry->create_time);
  StoreFileTime(&buf[3], entry->access_time);
  StoreFileTime(&buf[5], entry->write_time);
  buf[7] = static_cast<u32>(entry->size >> 32);
  buf[8] = static_cast<u32>(entry->size & 0xFFFFFFFF);
  return 1;
}

REX_XAM_SHIM_FN(SetFilePointerEx, u32, u32 hFile, u32 dist_high, u32 dist_low,
                MappedVoid lpNewFilePointer, u32 dwMoveMethod) {
  auto file = CurrentTitle().objects.Get<rex::kernel::IOpenedFile>(hFile);
  if (!file)
    return 0;

  i64 distance = static_cast<i64>((static_cast<u64>(dist_high) << 32) | dist_low);

  if (dwMoveMethod > kFileEnd)
    return 0;
  const u64 new_pos = file->Seek(distance, static_cast<rex::kernel::SeekOrigin>(dwMoveMethod));

  if (lpNewFilePointer) {
    auto* out = lpNewFilePointer.AsArray<u32>();
    out[0] = static_cast<u32>(new_pos & 0xFFFFFFFF);
    out[1] = static_cast<u32>(new_pos >> 32);
  }
  return 1;
}

REX_XAM_SHIM_FN(SetFileTime, u32, u32 hFile, MappedVoid lpCreationTime, MappedVoid lpLastAccessTime,
                MappedVoid lpLastWriteTime) {
  // The filesystem cannot modify timestamps, so report success.
  (void)hFile;
  (void)lpCreationTime;
  (void)lpLastAccessTime;
  (void)lpLastWriteTime;
  return 1;
}

REX_XAM_SHIM_FN(CompareFileTime, u32, MappedVoid lpFileTime1, MappedVoid lpFileTime2) {
  auto* ft1 = lpFileTime1.AsArray<u32>();
  auto* ft2 = lpFileTime2.AsArray<u32>();
  const u64 t1 = LoadFileTime(ft1);
  const u64 t2 = LoadFileTime(ft2);
  if (t1 < t2)
    return static_cast<u32>(-1);
  if (t1 > t2)
    return 1u;
  return 0u;
}

REX_XAM_SHIM_FN(CopyFileA, u32, MappedString lpExistingFileName, MappedString lpNewFileName,
                u32 bFailIfExists) {
  const char* src = static_cast<const char*>(lpExistingFileName);
  const char* dst = static_cast<const char*>(lpNewFileName);

  filesystem::OpenOptions source_options;
  source_options.disposition = filesystem::Disposition::kOpen;
  source_options.access = filesystem::Access::kReadData;
  source_options.require_file = true;
  auto src_file = CurrentTitle().file_system.Open(src, source_options);
  if (!src_file) {
    REXXAM_DEBUG("rexshim_CopyFileA: failed to open source '{}'", src);
    return 0;
  }

  filesystem::OpenOptions destination_options;
  destination_options.disposition =
      bFailIfExists ? filesystem::Disposition::kCreate : filesystem::Disposition::kOverwriteIf;
  destination_options.access = filesystem::Access::kWriteData;
  destination_options.require_file = true;
  auto dst_file = CurrentTitle().file_system.Open(dst, destination_options);
  if (!dst_file) {
    REXXAM_DEBUG("rexshim_CopyFileA: failed to open dest '{}'", dst);
    return 0;
  }

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

  REXXAM_TRACE("rexshim_CopyFileA: '{}' -> '{}' {}", src, dst, ok ? "OK" : "FAILED");
  return ok ? 1u : 0u;
}

u32 GetDiskFreeSpaceExA_entry(MappedString lpDirectoryName, MappedVoid lpFreeBytesAvailableToCaller,
                              MappedVoid lpTotalNumberOfBytes,
                              MappedVoid lpTotalNumberOfFreeBytes) {
  const char* path = static_cast<const char*>(lpDirectoryName);
  auto* fs = (&CurrentTitle().file_system);
  auto volume = fs->GetVolume(path);
  if (!fs->Stat(path) || !volume) {
    REXXAM_DEBUG("rexshim_GetDiskFreeSpaceExA: path not found '{}'", path);
    return 0;
  }

  u64 bytes_per_allocation_unit =
      static_cast<u64>(volume->sectors_per_allocation_unit) * volume->bytes_per_sector;
  u64 total_bytes = static_cast<u64>(volume->total_allocation_units) * bytes_per_allocation_unit;
  u64 free_bytes = static_cast<u64>(volume->available_allocation_units) * bytes_per_allocation_unit;

  if (lpFreeBytesAvailableToCaller) {
    auto* out = lpFreeBytesAvailableToCaller.AsArray<u32>();
    out[0] = static_cast<u32>(free_bytes & 0xFFFFFFFF);
    out[1] = static_cast<u32>(free_bytes >> 32);
  }
  if (lpTotalNumberOfBytes) {
    auto* out = lpTotalNumberOfBytes.AsArray<u32>();
    out[0] = static_cast<u32>(total_bytes & 0xFFFFFFFF);
    out[1] = static_cast<u32>(total_bytes >> 32);
  }
  if (lpTotalNumberOfFreeBytes) {
    auto* out = lpTotalNumberOfFreeBytes.AsArray<u32>();
    out[0] = static_cast<u32>(free_bytes & 0xFFFFFFFF);
    out[1] = static_cast<u32>(free_bytes >> 32);
  }

  REXXAM_TRACE("rexshim_GetDiskFreeSpaceExA: '{}' total={}MB free={}MB", path, total_bytes >> 20,
               free_bytes >> 20);
  return 1;
}

}  // namespace rex::xam

REX_XAM_BODY(__imp__CreateFileA, rex::xam::CreateFileA_shim)
REX_XAM_BODY(__imp__ReadFile, rex::xam::ReadFile_shim)
REX_XAM_BODY(__imp__WriteFile, rex::xam::WriteFile_shim)
REX_XAM_BODY(__imp__SetFilePointer, rex::xam::SetFilePointer_shim)
REX_XAM_BODY(__imp__GetFileSize, rex::xam::GetFileSize_shim)
REX_XAM_BODY(__imp__GetFileSizeEx, rex::xam::GetFileSizeEx_shim)
REX_XAM_BODY(__imp__SetEndOfFile, rex::xam::SetEndOfFile_shim)
REX_XAM_BODY(__imp__FlushFileBuffers, rex::xam::FlushFileBuffers_shim)
REX_XAM_BODY(__imp__DeleteFileA, rex::xam::DeleteFileA_shim)
REX_XAM_BODY(__imp__CloseHandle, rex::xam::CloseHandle_shim)
REX_XAM_BODY(__imp__FindFirstFileA, rex::xam::FindFirstFileA_shim)
REX_XAM_BODY(__imp__FindNextFileA, rex::xam::FindNextFileA_shim)
REX_XAM_BODY(__imp__CreateDirectoryA, rex::xam::CreateDirectoryA_shim)
REX_XAM_BODY(__imp__GetFileAttributesA, rex::xam::GetFileAttributesA_shim)
REX_XAM_BODY(__imp__GetFileAttributesExA, rex::xam::GetFileAttributesExA_shim)
REX_XAM_BODY(__imp__SetFilePointerEx, rex::xam::SetFilePointerEx_shim)
REX_XAM_BODY(__imp__SetFileTime, rex::xam::SetFileTime_shim)
REX_XAM_BODY(__imp__CompareFileTime, rex::xam::CompareFileTime_shim)
REX_XAM_BODY(__imp__CopyFileA, rex::xam::CopyFileA_shim)
REX_XAM_BODY(__imp__GetDiskFreeSpaceExA, rex::xam::GetDiskFreeSpaceExA_entry)
