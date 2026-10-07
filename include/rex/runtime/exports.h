/**
 * @file        runtime/exports.h
 * @brief       Kernel export tables and the lookups over them
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

#include <rex/macros.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

namespace rex::runtime {

/** A kernel module that serves an export table. */
enum class ExportModule : u8 {
  Xboxkrnl,
  XAM,
  XBDM,
};

/** The kind of an export, code or data. */
enum class ExportKind : u8 {
  Function,
  Variable,
};

/** One row of an export table. */
struct ExportEntry {
  u16 ordinal;           /**< The ordinal titles import by. */
  ExportKind kind;       /**< Code or data. */
  std::string_view name; /**< The export name. */
  ::PPCFunc* fn;         /**< The translation entry point, null for a variable. */
};

/** Addresses of the variable exports a kernel module has created. */
class ExportVariables {
 public:
  /**
   * Binds a variable export to the guest address that holds it.
   *
   * @param ordinal The variable ordinal.
   * @param address The guest address of the variable.
   */
  void Set(u16 ordinal, u32 address) { addresses_[ordinal] = address; }

  /**
   * Returns the address bound to a variable export.
   *
   * @param ordinal The variable ordinal.
   * @return The guest address, or 0 when the ordinal is not bound.
   */
  u32 Get(u16 ordinal) const {
    const auto it = addresses_.find(ordinal);
    return it == addresses_.end() ? 0 : it->second;
  }

 private:
  std::unordered_map<u16, u32> addresses_;
};

/**
 * Returns the rows of a module table, sorted by ordinal.
 *
 * @param module The module.
 * @return The table rows.
 */
REX_RUNTIME_API std::span<const ExportEntry> Exports(ExportModule module);

/**
 * Finds a row by ordinal.
 *
 * @param module The module.
 * @param ordinal The ordinal.
 * @return The row, or null when the module has no such ordinal.
 */
REX_RUNTIME_API const ExportEntry* FindByOrdinal(ExportModule module, u16 ordinal);

/**
 * Maps a module name to a table.
 *
 * Accepts the short form (`xam`) and any name that begins with the long form (`xam.xex`),
 * ignoring case.
 *
 * @param name The module name.
 * @return The module, or nothing when the name is not a kernel module.
 */
REX_RUNTIME_API std::optional<ExportModule> ModuleFromName(std::string_view name);

}  // namespace rex::runtime
