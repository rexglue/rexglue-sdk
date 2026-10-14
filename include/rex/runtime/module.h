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

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/macros.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/binary_types.h>

namespace rex::runtime {

class FunctionDispatcher;

/** A loaded executable image and the sections and symbols it exposes. */
class REX_RUNTIME_API Module {
 public:
  /**
   * Builds a module.
   *
   * @param function_dispatcher The dispatcher that supplies the memory, may be null.
   */
  explicit Module(FunctionDispatcher* function_dispatcher);
  /** Destroys the module. */
  virtual ~Module();

  runtime::Memory* GetMemory() const { return memory_; }
  FunctionDispatcher* GetFunctionDispatcher() const { return function_dispatcher_; }

  /**
   * Returns the module name.
   *
   * @return The name.
   */
  virtual const std::string& GetName() const = 0;
  /**
   * Reports whether the module is the title executable.
   *
   * @return True when it is.
   */
  virtual bool IsExecutable() const = 0;

  virtual u32 GetBaseAddress() const { return 0; }
  virtual u32 GetImageSize() const { return 0; }
  virtual u32 GetEntryPoint() const { return 0; }
  virtual u32 GetExportTableAddress() const { return 0; }

  /**
   * Returns the RVA of the PDATA table from the PE optional header exception data directory.
   *
   * @return The RVA, 0 by default.
   */
  virtual u32 GetExceptionDirectoryRva() const { return 0; }
  /**
   * Returns the size of the PDATA table from the PE optional header exception data directory.
   *
   * @return The size in bytes, 0 by default.
   */
  virtual u32 GetExceptionDirectorySize() const { return 0; }
  /**
   * Returns the guest address of the PDATA table from the PE optional header exception data
   * directory.
   *
   * @return The guest address, 0 by default.
   */
  virtual u32 GetExceptionDirectoryAddress() const { return 0; }

  /**
   * Reports whether an address is in an executable section.
   *
   * @param address The guest address.
   * @return True when the section is executable.
   */
  bool IsExecutableSection(u32 address) const;

  /**
   * Returns the binary sections, empty by default.
   *
   * @return The sections.
   */
  virtual std::span<const BinarySection> GetBinarySections() const;
  /**
   * Returns the section containing an address.
   *
   * @param address The guest address.
   * @return The section, or null when none contains the address.
   */
  virtual const BinarySection* FindSectionByAddress(u32 address) const;

  /**
   * Returns the binary symbols, empty by default.
   *
   * @return The symbols.
   */
  virtual std::span<const BinarySymbol> GetBinarySymbols() const;

 protected:
  /** Dispatcher that resolves the functions of the module, null until the module is set up. */
  FunctionDispatcher* function_dispatcher_ = nullptr;
  runtime::Memory* memory_ = nullptr; /**< Guest memory the module image is mapped into. */

  /** Populated by derived classes. */
  std::vector<BinarySection> binary_sections_;
  /** Symbols of the module, returned by GetBinarySymbols(). */
  std::vector<BinarySymbol> binary_symbols_;
};

}  // namespace rex::runtime
