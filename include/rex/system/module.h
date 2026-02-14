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

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/memory.h>
#include <rex/system/binary_types.h>
#include <rex/system/function.h>
#include <rex/system/symbol.h>
#include <rex/thread/mutex.h>

namespace rex::runtime {

class Processor;

class Module {
 public:
  explicit Module(Processor* processor);
  virtual ~Module();

  memory::Memory* memory() const { return memory_; }
  Processor* processor() const { return processor_; }

  virtual const std::string& name() const = 0;
  virtual bool is_executable() const = 0;

  virtual bool ContainsAddress(uint32_t address);

  Symbol* LookupSymbol(uint32_t address, bool wait = true);
  virtual Symbol::Status DeclareFunction(uint32_t address, Function** out_function);
  virtual Symbol::Status DeclareVariable(uint32_t address, Symbol** out_symbol);

  Symbol::Status DefineFunction(Function* symbol);
  Symbol::Status DefineVariable(Symbol* symbol);

  void ForEachFunction(std::function<void(Function*)> callback);
  void ForEachSymbol(size_t start_index, size_t end_index, std::function<void(Symbol*)> callback);
  size_t QuerySymbolCount();

  bool ReadMap(const char* file_name);

  // Binary introspection interface (virtual with defaults for backwards compat)
  virtual uint32_t base_address() const { return 0; }
  virtual uint32_t image_size() const { return 0; }
  virtual uint32_t entry_point() const { return 0; }
  virtual uint32_t export_table_address() const { return 0; }

  // Exception DataDirectory accessors (for PDATA table)
  // These return the correct PDATA location from the PE Optional Header.
  virtual uint32_t exception_directory_rva() const { return 0; }
  virtual uint32_t exception_directory_size() const { return 0; }
  virtual uint32_t exception_directory_address() const { return 0; }

  // Check if address is in an executable section
  bool isExecutableSection(uint32_t address) const;

  // Section access - default implementations return empty
  virtual std::span<const BinarySection> binary_sections() const;
  virtual const BinarySection* FindSectionByName(std::string_view name) const;
  virtual const BinarySection* FindSectionByAddress(uint32_t address) const;

  // Symbol access - default implementations return empty
  virtual std::span<const BinarySymbol> binary_symbols() const;
  virtual const BinarySymbol* FindSymbolByName(std::string_view name) const;
  virtual const BinarySymbol* FindSymbolByAddress(uint32_t address) const;
  virtual const BinarySymbol* FindSymbolContainingAddress(uint32_t address) const;

  // Symbol manipulation helpers (for external symbol loading, e.g., from map files)
  void AddBinarySymbol(BinarySymbol symbol);
  void ClearBinarySymbols();

 protected:
  virtual std::unique_ptr<Function> CreateFunction(uint32_t address) = 0;

  Processor* processor_ = nullptr;
  memory::Memory* memory_ = nullptr;

  // Storage for binary introspection (populated by derived classes)
  std::vector<BinarySection> binary_sections_;
  std::vector<BinarySymbol> binary_symbols_;

 private:
  Symbol::Status DeclareSymbol(Symbol::Type type, uint32_t address, Symbol** out_symbol);
  Symbol::Status DefineSymbol(Symbol* symbol);

  rex::thread::global_critical_region global_critical_region_;
  // TODO(benvanik): replace with a better data structure.
  std::unordered_map<uint32_t, Symbol*> map_;
  std::vector<std::unique_ptr<Symbol>> list_;
};

}  // namespace rex::runtime
