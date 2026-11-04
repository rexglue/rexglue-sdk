/**
 * @file        kernel/shim/file.cpp
 * @brief       Shim file routines that have no XAM ordinal
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

#include <rex/logging/log_category.h>
#include "kernel/categories.h"
#include <rex/memory.h>
#include <rex/ppc/hook.h>
#include "kernel/shim/function.h"
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>
#include "kernel/title_state.h"
#include "kernel/kernel.h"

using rex::kernel::X_STATUS;

namespace rex::kernel::shim {

REX_SHIM_FN(FindClose, u32, u32 hFindFile) {
  if (hFindFile == kXInvalidHandleValue || hFindFile == 0)
    return 0;
  if (!kernel::GetCurrent().GetObjectTable().Close(hFindFile)) {
    REXKRNL_WARN("rexshim_FindClose: unknown handle {:#x}", hFindFile);
    return 0;
  }
  return 1;
}

REX_SHIM_FN(MoveFileA, u32, MappedString lpExistingFileName, MappedString lpNewFileName) {
  const char* src = static_cast<const char*>(lpExistingFileName);
  const char* dst = static_cast<const char*>(lpNewFileName);

  if (auto renamed = kernel::GetCurrentState().GetFileSystem()->Rename(src, dst, false); !renamed) {
    REXKRNL_DEBUG("rexshim_MoveFileA: rename failed '{}' -> '{}': {:#x}", src, dst,
                  rex::kernel::ToXStatus(renamed.error(), X_STATUS_NOT_SUPPORTED));
    return 0;
  }
  REXKRNL_TRACE("rexshim_MoveFileA: '{}' -> '{}'", src, dst);
  return 1;
}

REX_SHIM_FN(SetFileAttributesA, u32, MappedString lpFileName, u32 dwFileAttributes) {
  (void)lpFileName;
  (void)dwFileAttributes;
  return 1;
}

REX_SHIM_FN(RemoveDirectoryA, u32, MappedString lpPathName) {
  const char* path = static_cast<const char*>(lpPathName);
  bool ok = kernel::GetCurrentState().GetFileSystem()->Remove(path).has_value();
  if (!ok)
    REXKRNL_DEBUG("rexshim_RemoveDirectoryA: FAILED '{}'", path);
  return ok ? 1u : 0u;
}

REX_SHIM_FN(GetFileType, u32, u32 hFile) {
  auto file = kernel::GetCurrent().GetObjectTable().Get<rex::kernel::FileObject>(hFile);
  if (!file)
    // FILE_TYPE_UNKNOWN
    return 0;
  // FILE_TYPE_DISK
  return 1;
}

}  // namespace rex::kernel::shim

REX_EXPORT(__imp__MoveFileA, rex::kernel::shim::MoveFileA_shim)
REX_EXPORT(__imp__SetFileAttributesA, rex::kernel::shim::SetFileAttributesA_shim)
