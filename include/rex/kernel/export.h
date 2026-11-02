/**
 * @file        kernel/export.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's export_resolver (Ben Vanik, 2020).
 */

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include <rex/platform.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/types.h>

namespace rex::kernel {

enum class ExportModule : u8 {
  kXboxkrnl,
  kXAM,
  kXBDM,
};

enum class ExportType : u8 {
  kFunction, /**< A translated function, `ExportEntry::fn` is set. */
  kVariable, /**< A data export, `ExportEntry::fn` is null. */
};

struct ExportEntry {
  u16 ordinal; /**< The ordinal titles import by. */
  ExportType type;
  std::string_view name;
  ::PPCFunc* fn; /**< The translation entry point, null for a variable. */
};

class ExportVariableTable {
 public:
  /** Binds a variable export to the guest address that holds it. */
  void SetAddress(u16 ordinal, u32 address) { addresses_[ordinal] = address; }

  /** Returns the address bound to a variable export, or 0 when the ordinal is not bound. */
  u32 GetAddress(u16 ordinal) const {
    const auto it = addresses_.find(ordinal);
    return it == addresses_.end() ? 0 : it->second;
  }

 private:
  std::unordered_map<u16, u32> addresses_;
};

/** Returns the rows of a module table, sorted by ordinal. */
REX_KERNEL_API std::span<const ExportEntry> GetExports(ExportModule module);

/**
 * Finds a row by ordinal.
 *
 * @return The row, or null when the module has no such ordinal.
 */
REX_KERNEL_API const ExportEntry* FindByOrdinal(ExportModule module, u16 ordinal);

/**
 * Reports whether an export never returns to its caller.
 *
 * @return True when a call to the export ends the calling thread, or restarts the console.
 */
REX_KERNEL_API bool IsNoReturnExport(ExportModule module, u16 ordinal);

/**
 * Maps a module name to a table.
 *
 * Accepts `xam` or any name that begins with `xam.xex`, ignoring case.
 *
 * @return The module, or nothing when the name is not a kernel module.
 */
REX_KERNEL_API std::optional<ExportModule> ModuleFromName(std::string_view name);

}  // namespace rex::kernel
