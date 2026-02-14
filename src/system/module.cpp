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

#include <algorithm>
#include <fstream>
#include <sstream>  // NOLINT(readability/streams): should be replaced.
#include <string>

#include <rex/dbg.h>
#include <rex/system/function.h>
#include <rex/system/module.h>
#include <rex/system/processor.h>
#include <rex/thread.h>

namespace rex::runtime {

Module::Module(Processor* processor)
    : processor_(processor), memory_(processor ? processor->memory() : nullptr) {}

Module::~Module() = default;

bool Module::ContainsAddress(uint32_t address) {
  (void)address;
  return true;
}

Symbol* Module::LookupSymbol(uint32_t address, bool wait) {
  auto global_lock = global_critical_region_.Acquire();
  const auto it = map_.find(address);
  Symbol* symbol = it != map_.end() ? it->second : nullptr;
  if (symbol) {
    if (symbol->status() == Symbol::Status::kDeclaring) {
      // Some other thread is declaring the symbol - wait.
      if (wait) {
        do {
          global_lock.unlock();
          // TODO(benvanik): sleep for less time?
          rex::thread::Sleep(std::chrono::microseconds(100));
          global_lock.lock();
        } while (symbol->status() == Symbol::Status::kDeclaring);
      } else {
        // Immediate request, just return.
        symbol = nullptr;
      }
    }
  }
  global_lock.unlock();
  return symbol;
}

Symbol::Status Module::DeclareSymbol(Symbol::Type type, uint32_t address, Symbol** out_symbol) {
  *out_symbol = nullptr;
  auto global_lock = global_critical_region_.Acquire();
  auto it = map_.find(address);
  Symbol* symbol = it != map_.end() ? it->second : nullptr;
  Symbol::Status status;
  if (symbol) {
    // If we exist but are the wrong type, die.
    if (symbol->type() != type) {
      global_lock.unlock();
      return Symbol::Status::kFailed;
    }
    // If we aren't ready yet spin and wait.
    if (symbol->status() == Symbol::Status::kDeclaring) {
      // Still declaring, so spin.
      do {
        global_lock.unlock();
        // TODO(benvanik): sleep for less time?
        rex::thread::Sleep(std::chrono::microseconds(100));
        global_lock.lock();
      } while (symbol->status() == Symbol::Status::kDeclaring);
    }
    status = symbol->status();
  } else {
    // Create and return for initialization.
    switch (type) {
      case Symbol::Type::kFunction:
        symbol = CreateFunction(address).release();
        break;
      case Symbol::Type::kVariable:
        symbol = new Symbol(Symbol::Type::kVariable, this, address);
        break;
    }
    map_[address] = symbol;
    list_.emplace_back(symbol);
    status = Symbol::Status::kNew;
  }
  global_lock.unlock();
  *out_symbol = symbol;

  // Get debug info from providers, if this is new.
  if (status == Symbol::Status::kNew) {
    // TODO(benvanik): lookup in map data/dwarf/etc?
  }

  return status;
}

Symbol::Status Module::DeclareFunction(uint32_t address, Function** out_function) {
  Symbol* symbol;
  Symbol::Status status = DeclareSymbol(Symbol::Type::kFunction, address, &symbol);
  *out_function = static_cast<Function*>(symbol);
  return status;
}

Symbol::Status Module::DeclareVariable(uint32_t address, Symbol** out_symbol) {
  Symbol::Status status = DeclareSymbol(Symbol::Type::kVariable, address, out_symbol);
  return status;
}

Symbol::Status Module::DefineSymbol(Symbol* symbol) {
  auto global_lock = global_critical_region_.Acquire();
  Symbol::Status status;
  if (symbol->status() == Symbol::Status::kDeclared) {
    // Declared but undefined, so request caller define it.
    symbol->set_status(Symbol::Status::kDefining);
    status = Symbol::Status::kNew;
  } else if (symbol->status() == Symbol::Status::kDefining) {
    // Still defining, so spin.
    do {
      global_lock.unlock();
      // TODO(benvanik): sleep for less time?
      rex::thread::Sleep(std::chrono::microseconds(100));
      global_lock.lock();
    } while (symbol->status() == Symbol::Status::kDefining);
    status = symbol->status();
  } else {
    status = symbol->status();
  }
  global_lock.unlock();
  return status;
}

Symbol::Status Module::DefineFunction(Function* symbol) {
  return DefineSymbol(symbol);
}

Symbol::Status Module::DefineVariable(Symbol* symbol) {
  return DefineSymbol(symbol);
}

void Module::ForEachFunction(std::function<void(Function*)> callback) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& symbol : list_) {
    if (symbol->type() == Symbol::Type::kFunction) {
      Function* info = static_cast<Function*>(symbol.get());
      callback(info);
    }
  }
}

void Module::ForEachSymbol(size_t start_index, size_t end_index,
                           std::function<void(Symbol*)> callback) {
  auto global_lock = global_critical_region_.Acquire();
  start_index = std::min(start_index, list_.size());
  end_index = std::min(end_index, list_.size());
  for (size_t i = start_index; i <= end_index; ++i) {
    auto& symbol = list_[i];
    callback(symbol.get());
  }
}

size_t Module::QuerySymbolCount() {
  auto global_lock = global_critical_region_.Acquire();
  return list_.size();
}

bool Module::ReadMap(const char* file_name) {
  // TODO(tomc): do we need this here? map loaded should be done
  // during analysis/recompilation mode in rexglue app.
  (void)file_name;
  return false;
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
