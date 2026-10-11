/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>

#include <rex/string.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime.h>

namespace rex::runtime {

size_t KernelModuleData::ThunkKeyHash::operator()(const ThunkKey& key) const noexcept {
  return std::hash<u64>{}((static_cast<u64>(key.caller_module_base) << 16) | key.ordinal);
}

X_HANDLE CreateKernelModule(Runtime& runtime, std::string_view path) {
  const X_HANDLE handle = CreateModule(runtime, ModuleType::Kernel);
  if (!handle) {
    return 0;
  }
  auto module = runtime.object_table().Get<ModuleObject>(handle);
  module->path = path;
  module->name = rex::utf8_find_base_name_from_guest_path(path);
  return handle;
}

void ModuleObject::InvalidateThunkCache(u32 lo, u32 hi) {
  std::erase_if(kernel.thunk_cache,
                [lo, hi](const auto& entry) { return entry.second >= lo && entry.second < hi; });
}

}  // namespace rex::runtime
