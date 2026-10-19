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

#include <rex/runtime/module.h>
#include <rex/runtime/function_dispatcher.h>

namespace rex::runtime {

Module::Module(FunctionDispatcher* function_dispatcher)
    : function_dispatcher_(function_dispatcher),
      memory_(function_dispatcher ? function_dispatcher->GetMemory() : nullptr) {}

Module::~Module() = default;

std::span<const BinarySection> Module::GetBinarySections() const {
  return binary_sections_;
}

const BinarySection* Module::FindSectionByAddress(u32 address) const {
  for (const auto& sec : binary_sections_) {
    if (address >= sec.virtual_address && address < sec.virtual_address + sec.virtual_size) {
      return &sec;
    }
  }
  return nullptr;
}

std::span<const BinarySymbol> Module::GetBinarySymbols() const {
  return binary_symbols_;
}

bool Module::IsExecutableAddress(u32 address) const {
  for (const auto& sec : binary_sections_) {
    if (address >= sec.virtual_address && address < sec.virtual_address + sec.virtual_size) {
      return sec.executable;
    }
  }
  return false;
}

}  // namespace rex::runtime
