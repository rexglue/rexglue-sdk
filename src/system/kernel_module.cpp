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

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime/exports.h>
#include <rex/runtime.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/platform/mutex.h>

namespace rex::system {

KernelModule::KernelModule(KernelState* kernel_state, const std::string_view path)
    : XModule(kernel_state, ModuleType::kKernelModule) {
  emulator_ = kernel_state->emulator();
  memory_ = emulator_->memory();

  path_ = path;
  name_ = rex::utf8_find_base_name_from_guest_path(path);

  // Persist this object through reloads.
  host_object_ = true;

  OnLoad();
}

KernelModule::~KernelModule() {}

uint32_t KernelModule::GetProcAddressByOrdinal(uint16_t ordinal, uint32_t caller_address) {
  const auto module = rex::runtime::ModuleFromName(name_);
  const auto* export_entry = module ? rex::runtime::FindByOrdinal(*module, ordinal) : nullptr;
  if (!export_entry) {
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: ordinal {:04X} not found in {}", ordinal, name_);
    return 0;
  }

  if (export_entry->kind == rex::runtime::ExportKind::Variable) {
    const uint32_t address = Variable(ordinal);
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) -> variable at {:08X}",
                 export_entry->name, ordinal, address);
    return address;
  }

  auto* dispatcher = emulator_->function_dispatcher();
  uint32_t caller_module_base = dispatcher->FindCallerModuleBase(caller_address);
  ThunkKey key{caller_module_base, ordinal};

  auto thunk_it = thunk_cache_.find(key);
  if (thunk_it != thunk_cache_.end()) {
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) in {} -> cached thunk {:08X}",
                 export_entry->name, ordinal, name_, thunk_it->second);
    return thunk_it->second;
  }

  uint32_t thunk_addr = dispatcher->AllocateThunk(export_entry->fn, caller_address);
  if (thunk_addr) {
    thunk_cache_[key] = thunk_addr;
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) in {} -> thunk at {:08X}",
                 export_entry->name, ordinal, name_, thunk_addr);
    return thunk_addr;
  }

  REXLOG_WARN(system, "GetProcAddressByOrdinal: function {} ({:04X}) in {} - no thunk available",
              export_entry->name, ordinal, name_);
  return 0;
}

uint32_t KernelModule::GetProcAddressByName(const std::string_view name) {
  // TODO: Does this even work for kernel modules?
  (void)name;
  REX_FAIL(system, 0, "KernelModule::GetProcAddressByName not implemented");
}

void KernelModule::InvalidateThunkCacheInRange(uint32_t lo, uint32_t hi) {
  for (auto it = thunk_cache_.begin(); it != thunk_cache_.end();) {
    if (it->second >= lo && it->second < hi) {
      it = thunk_cache_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace rex::system
