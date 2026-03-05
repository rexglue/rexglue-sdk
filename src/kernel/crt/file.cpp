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

#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
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

static rex::system::KernelState* KS() {
  return rex::system::kernel_state();
}

ppc_u32_result_t CreateFileA_entry(ppc_pchar_t lpFileName, ppc_u32_t dwDesiredAccess,
                                   ppc_u32_t dwShareMode, ppc_pvoid_t lpSecurityAttributes,
                                   ppc_u32_t dwCreationDisposition, ppc_u32_t dwFlagsAndAttributes,
                                   ppc_u32_t hTemplateFile) {
  const char* path = static_cast<const char*>(lpFileName);
  auto* ks = KS();
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
  auto file = KS()->object_table()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
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
  auto file = KS()->object_table()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
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
  auto file = KS()->object_table()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
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
  auto file = KS()->object_table()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
  if (!file)
    return kInvalidHandleValue;

  uint64_t size = file->entry()->size();
  if (lpFileSizeHigh)
    *lpFileSizeHigh = static_cast<uint32_t>(size >> 32);
  return static_cast<uint32_t>(size & 0xFFFFFFFF);
}

ppc_u32_result_t GetFileSizeEx_entry(ppc_u32_t hFile, ppc_pvoid_t lpFileSize) {
  auto file = KS()->object_table()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
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
  auto file = KS()->object_table()->LookupObject<rex::system::XFile>(static_cast<uint32_t>(hFile));
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
  bool ok = KS()->file_system()->DeletePath(path);
  if (!ok)
    REXKRNL_DEBUG("rexcrt_DeleteFileA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

ppc_u32_result_t CloseHandle_entry(ppc_u32_t hObject) {
  uint32_t h = static_cast<uint32_t>(hObject);
  if (h == kInvalidHandleValue || h == 0)
    return 0;
  auto status = KS()->object_table()->ReleaseHandle(h);
  if (XFAILED(status)) {
    REXKRNL_WARN("rexcrt_CloseHandle: unknown handle {:#x}", h);
    return 0;
  }
  return 1;
}

ppc_u32_result_t FindFirstFileA_entry(ppc_pchar_t lpFileName, ppc_pvoid_t lpFindFileData) {
  REXKRNL_WARN("rexcrt_FindFirstFileA: STUB called for '{}'", static_cast<const char*>(lpFileName));
  return kInvalidHandleValue;
}

ppc_u32_result_t FindNextFileA_entry(ppc_u32_t hFindFile, ppc_pvoid_t lpFindFileData) {
  REXKRNL_WARN("rexcrt_FindNextFileA: STUB");
  return 0;
}

ppc_u32_result_t CreateDirectoryA_entry(ppc_pchar_t lpPathName, ppc_pvoid_t lpSecurityAttributes) {
  const char* path = static_cast<const char*>(lpPathName);
  auto* entry = KS()->file_system()->CreatePath(path, rex::filesystem::kFileAttributeDirectory);
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
REXCRT_EXPORT(rexcrt_CreateDirectoryA, rex::kernel::crt::CreateDirectoryA_entry)
REXCRT_EXPORT(rexcrt_MoveFileA, rex::kernel::crt::MoveFileA_entry)
REXCRT_EXPORT(rexcrt_SetFileAttributesA, rex::kernel::crt::SetFileAttributesA_entry)
