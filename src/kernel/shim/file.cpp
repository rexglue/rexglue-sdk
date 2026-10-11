/**
 * @file        kernel/shim/file.cpp
 * @brief       Shim file routines that have no Xam ordinal
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
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/file_object.h>
#include <rex/runtime/xtypes.h>

using rex::X_STATUS;

namespace rex::kernel::shim {

constexpr u32 kInvalidHandleValue = 0xFFFFFFFF;

REX_SHIM_FN(FindClose, u32, u32 hFindFile) {
  if (hFindFile == kInvalidHandleValue || hFindFile == 0)
    return 0;
  if (!runtime::Current().object_table().Close(hFindFile)) {
    REXLOG_WARN(kernel, "rexshim_FindClose: unknown handle {:#x}", hFindFile);
    return 0;
  }
  return 1;
}

REX_SHIM_FN(MoveFileA, u32, mapped_string lpExistingFileName, mapped_string lpNewFileName) {
  const char* src = static_cast<const char*>(lpExistingFileName);
  const char* dst = static_cast<const char*>(lpNewFileName);

  if (auto renamed = runtime::Current().file_system()->Rename(src, dst, false); !renamed) {
    REXLOG_DEBUG(kernel, "rexshim_MoveFileA: rename failed '{}' -> '{}': {:#x}", src, dst,
                 rex::runtime::ToXStatus(renamed.error(), X_STATUS_NOT_SUPPORTED));
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
  bool ok = runtime::Current().file_system()->Remove(path).has_value();
  if (!ok)
    REXLOG_DEBUG(kernel, "rexshim_RemoveDirectoryA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

REX_SHIM_FN(GetFileType, u32, u32 hFile) {
  auto file = runtime::Current().object_table().Get<rex::runtime::FileObject>(hFile);
  if (!file)
    // FILE_TYPE_UNKNOWN
    return 0;
  // FILE_TYPE_DISK
  return 1;
}

}  // namespace rex::kernel::shim

REX_EXPORT(__imp__MoveFileA, rex::kernel::shim::MoveFileA_shim)
REX_EXPORT(__imp__SetFileAttributesA, rex::kernel::shim::SetFileAttributesA_shim)
