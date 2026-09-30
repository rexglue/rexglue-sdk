/**
 * @file        kernel/shim/file.cpp
 *
 * @brief       Shim file routines that have no Xam ordinal
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
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

namespace rex::kernel::shim {

constexpr u32 kInvalidHandleValue = 0xFFFFFFFF;

REX_SHIM_FN(FindClose, u32, u32 hFindFile) {
  if (hFindFile == kInvalidHandleValue || hFindFile == 0)
    return 0;
  auto status = REX_KERNEL_OBJECTS()->ReleaseHandle(hFindFile);
  if (XFAILED(status)) {
    REXLOG_WARN(kernel, "rexshim_FindClose: unknown handle {:#x}", hFindFile);
    return 0;
  }
  return 1;
}

REX_SHIM_FN(MoveFileA, u32, mapped_string lpExistingFileName, mapped_string lpNewFileName) {
  const char* src = static_cast<const char*>(lpExistingFileName);
  const char* dst = static_cast<const char*>(lpNewFileName);

  auto* fs = REX_KERNEL_FS();
  if (!fs->Stat(src)) {
    REXLOG_DEBUG(kernel, "rexshim_MoveFileA: source not found '{}'", src);
    return 0;
  }
  // Win32 MoveFileA fails if the destination already exists; callers wanting
  // overwrite semantics use MoveFileExA with MOVEFILE_REPLACE_EXISTING.
  if (fs->Stat(dst)) {
    REXLOG_DEBUG(kernel, "rexshim_MoveFileA: destination exists '{}'", dst);
    return 0;
  }

  if (auto renamed = fs->Rename(src, dst); !renamed) {
    REXLOG_DEBUG(kernel, "rexshim_MoveFileA: rename failed '{}' -> '{}': {:#x}", src, dst,
                 rex::system::ToXStatus(renamed.error(), X_STATUS_NOT_SUPPORTED));
    return 0;
  }
  REXLOG_TRACE(kernel, "rexshim_MoveFileA: '{}' -> '{}'", src, dst);
  return 1;
}

REX_SHIM_FN(SetFileAttributesA, u32, mapped_string lpFileName, u32 dwFileAttributes) {
  (void)lpFileName;
  (void)dwFileAttributes;
  return 1;
}

REX_SHIM_FN(RemoveDirectoryA, u32, mapped_string lpPathName) {
  const char* path = static_cast<const char*>(lpPathName);
  bool ok = REX_KERNEL_FS()->Remove(path).has_value();
  if (!ok)
    REXLOG_DEBUG(kernel, "rexshim_RemoveDirectoryA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

REX_SHIM_FN(GetFileType, u32, u32 hFile) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(static_cast<u32>(hFile));
  if (!file)
    return 0;  // FILE_TYPE_UNKNOWN
  return 1;    // FILE_TYPE_DISK
}

}  // namespace rex::kernel::shim

REX_EXPORT(__imp__MoveFileA, rex::kernel::shim::MoveFileA_shim)
REX_EXPORT(__imp__SetFileAttributesA, rex::kernel::shim::SetFileAttributesA_shim)
