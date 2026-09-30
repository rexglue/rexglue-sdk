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
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmodule.h>

namespace rex::system {

XModule::XModule(KernelState* kernel_state, ModuleType module_type)
    : XObject(kernel_state, kObjectType),
      module_type_(module_type),
      processor_module_(nullptr),
      hmodule_ptr_(0) {
  // Loader data (HMODULE)
  hmodule_ptr_ = memory()->SystemHeapAlloc(sizeof(X_LDR_DATA_TABLE_ENTRY));

  // Hijack the checksum field to store our kernel object handle.
  auto ldr_data = memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule_ptr_);
  ldr_data->checksum = handle();
}

XModule::~XModule() {
  kernel_state_->UnregisterModule(this);

  // Destroy the loader data.
  memory()->SystemHeapFree(hmodule_ptr_);
}

bool XModule::Matches(const std::string_view name) const {
  return rex::utf8_equal_case(rex::utf8_find_name_from_guest_path(path()), name) ||
         rex::utf8_equal_case(this->name(), name) || rex::utf8_equal_case(path(), name);
}  // namespace system

void XModule::OnLoad() {
  kernel_state_->RegisterModule(this);
}

void XModule::OnUnload() {
  kernel_state_->UnregisterModule(this);
}

X_STATUS XModule::GetSection(const std::string_view name, uint32_t* out_section_data,
                             uint32_t* out_section_size) {
  return X_STATUS_UNSUCCESSFUL;
}

object_ref<XModule> XModule::GetFromHModule(KernelState* kernel_state, void* hmodule) {
  // Grab the object from our stashed kernel handle
  return kernel_state->object_table()->LookupObject<XModule>(GetHandleFromHModule(hmodule));
}

uint32_t XModule::GetHandleFromHModule(void* hmodule) {
  auto ldr_data = reinterpret_cast<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
  return ldr_data->checksum;
}

}  // namespace rex::system
