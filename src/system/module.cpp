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

#include <utility>

#include <rex/system/module.h>
#include <rex/system/processor.h>

namespace rex::runtime {

Module::Module(Processor* processor)
    : processor_(processor), memory_(processor ? processor->memory() : nullptr) {}

Module::~Module() = default;

bool Module::ContainsAddress(uint32_t address) {
  (void)address;
  return true;
}

// Binary introspection default implementations
std::span<const BinarySection> Module::binary_sections() const {
  return binary_sections_;
}

const BinarySection* Module::FindSectionByName(std::string_view name) const {
  for (const auto& sec : binary_sections_) {
    if (sec.name == name)
      return &sec;
  }
  return nullptr;
}

const BinarySection* Module::FindSectionByAddress(uint32_t address) const {
  for (const auto& sec : binary_sections_) {
    if (address >= sec.virtual_address && address < sec.virtual_address + sec.virtual_size) {
      return &sec;
    }
  }
  return nullptr;
}

std::span<const BinarySymbol> Module::binary_symbols() const {
  return binary_symbols_;
}

const BinarySymbol* Module::FindSymbolByName(std::string_view name) const {
  for (const auto& sym : binary_symbols_) {
    if (sym.name == name)
      return &sym;
  }
  return nullptr;
}

const BinarySymbol* Module::FindSymbolByAddress(uint32_t address) const {
  for (const auto& sym : binary_symbols_) {
    if (sym.address == address)
      return &sym;
  }
  return nullptr;
}

const BinarySymbol* Module::FindSymbolContainingAddress(uint32_t address) const {
  for (const auto& sym : binary_symbols_) {
    if (address >= sym.address && address < sym.address + sym.size) {
      return &sym;
    }
  }
  return nullptr;
}

void Module::AddBinarySymbol(BinarySymbol symbol) {
  binary_symbols_.push_back(std::move(symbol));
}

void Module::ClearBinarySymbols() {
  binary_symbols_.clear();
}

bool Module::isExecutableSection(uint32_t address) const {
  for (const auto& sec : binary_sections_) {
    if (address >= sec.virtual_address && address < sec.virtual_address + sec.virtual_size) {
      return sec.executable;
    }
  }
  return false;
}

}  // namespace rex::runtime
