/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/module.h>
#include <rex/kernel/function_dispatcher.h>

namespace rex::kernel {

GuestModule::GuestModule(FunctionDispatcher* function_dispatcher)
    : function_dispatcher_(function_dispatcher),
      memory_(function_dispatcher ? function_dispatcher->GetAddressSpace() : nullptr) {}

GuestModule::~GuestModule() = default;

std::span<const BinarySection> GuestModule::GetBinarySections() const {
  return binary_sections_;
}

const BinarySection* GuestModule::FindSectionByAddress(u32 address) const {
  for (const auto& sec : binary_sections_) {
    if (address >= sec.virtual_address && address < sec.virtual_address + sec.virtual_size) {
      return &sec;
    }
  }
  return nullptr;
}

std::span<const BinarySymbol> GuestModule::GetBinarySymbols() const {
  return binary_symbols_;
}

bool GuestModule::IsExecutableAddress(u32 address) const {
  for (const auto& sec : binary_sections_) {
    if (address >= sec.virtual_address && address < sec.virtual_address + sec.virtual_size) {
      return sec.executable;
    }
  }
  return false;
}

}  // namespace rex::kernel
