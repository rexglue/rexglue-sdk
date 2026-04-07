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
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/thread/mutex.h>

namespace rex::system {

KernelModule::KernelModule(KernelState* kernel_state, const std::string_view path)
    : XModule(kernel_state, ModuleType::kKernelModule) {
  emulator_ = kernel_state->emulator();
  memory_ = emulator_->memory();
  export_resolver_ = kernel_state->emulator()->export_resolver();

  path_ = path;
  name_ = rex::string::utf8_find_base_name_from_guest_path(path);

  // Persist this object through reloads.
  host_object_ = true;

  OnLoad();
}

KernelModule::~KernelModule() {}

uint32_t KernelModule::GetProcAddressByOrdinal(uint16_t ordinal) {
  // Look up the export in the resolver
  auto export_entry = export_resolver_->GetExportByOrdinal(name_, ordinal);
  if (!export_entry) {
    REXSYS_DEBUG("GetProcAddressByOrdinal: ordinal {:04X} not found in {}", ordinal, name_);
    return 0;
  }

  if (export_entry->type == rex::runtime::Export::Type::kVariable) {
    // Variables have guest addresses we can return directly
    REXSYS_DEBUG("GetProcAddressByOrdinal: {} ({:04X}) -> variable at {:08X}", export_entry->name,
                 ordinal, export_entry->variable_ptr);
    return export_entry->variable_ptr;
  }

  // Check thunk cache first (already allocated)
  auto thunk_it = thunk_cache_.find(ordinal);
  if (thunk_it != thunk_cache_.end()) {
    REXSYS_DEBUG("GetProcAddressByOrdinal: {} ({:04X}) in {} -> cached thunk {:08X}",
                 export_entry->name, ordinal, name_, thunk_it->second);
    return thunk_it->second;
  }

  // Look up native implementation by name from the auto-registry
  std::string imp_name = std::string("__imp__") + export_entry->name;
  REXSYS_DEBUG("GetProcAddressByOrdinal: searching registry for '{}'", imp_name);
  PPCFunc* func = rex::ppc::FindPPCFuncByName(imp_name.c_str());
  if (func) {
    auto* dispatcher = emulator_->function_dispatcher();
    uint32_t thunk_addr = dispatcher->AllocateThunk(func);
    if (thunk_addr) {
      thunk_cache_[ordinal] = thunk_addr;
      REXSYS_INFO("GetProcAddressByOrdinal: {} ({:04X}) in {} -> thunk at {:08X}",
                  export_entry->name, ordinal, name_, thunk_addr);
      return thunk_addr;
    }
  }

  // No native implementation available
  REXSYS_WARN("GetProcAddressByOrdinal: function {} ({:04X}) in {} - no native implementation",
              export_entry->name, ordinal, name_);
  return 0;
}

uint32_t KernelModule::GetProcAddressByName(const std::string_view name) {
  // TODO: Does this even work for kernel modules?
  (void)name;
  REXSYS_ERROR("KernelModule::GetProcAddressByName not implemented");
  return 0;
}

}  // namespace rex::system
