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
#include <string_view>

#include <rex/string/path.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/object_table.h>

namespace rex::kernel {

size_t KernelModuleState::ThunkKeyHash::operator()(const ThunkKey& key) const noexcept {
  return std::hash<u64>{}((static_cast<u64>(key.caller_module_base) << 16) | key.ordinal);
}

X_HANDLE ModuleObject::CreateKernelModule(TitleRuntime& runtime, std::string_view path) {
  const X_HANDLE handle = CreateModule(runtime, ModuleType::kKernel);
  if (!handle) {
    return 0;
  }
  auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
  module->path = path;
  module->name = rex::string::utf8_find_base_name_from_guest_path(path);
  return handle;
}

void ModuleObject::InvalidateThunkCache(u32 lo, u32 hi) {
  const auto in_range = [lo, hi](const auto& entry) {
    return entry.second >= lo && entry.second < hi;
  };
  std::erase_if(kernel.thunk_cache, in_range);
  std::erase_if(user.thunk_cache, in_range);
}

}  // namespace rex::kernel
