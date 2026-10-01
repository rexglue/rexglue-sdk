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
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>

namespace rex::system {

size_t KernelModuleData::ThunkKeyHash::operator()(const ThunkKey& key) const noexcept {
  return std::hash<u64>{}((static_cast<u64>(key.caller_module_base) << 16) | key.ordinal);
}

X_HANDLE CreateKernelModule(KernelState* kernel_state, std::string_view path) {
  const X_HANDLE handle = CreateModule(kernel_state, ModuleType::Kernel);
  if (!handle) {
    return 0;
  }
  auto module = kernel_state->object_table()->Get<ModuleObject>(handle);
  module->path = path;
  module->name = rex::utf8_find_base_name_from_guest_path(path);
  return handle;
}

void KernelModuleInvalidateThunkCacheInRange(ModuleObject& module, u32 lo, u32 hi) {
  std::erase_if(module.kernel.thunk_cache,
                [lo, hi](const auto& entry) { return entry.second >= lo && entry.second < hi; });
}

}  // namespace rex::system
