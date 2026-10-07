/**
 * @file        kernel/exports.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's export_resolver (Ben Vanik, 2020).
 */

#include <rex/runtime/exports.h>

#include <algorithm>

#include <rex/ppc/context.h>
#include <rex/string.h>

#define REX_EXPORT_ROW_kFunction(ordinal, text, symbol) symbol(::PPCContext& __restrict, u8*)
#define REX_EXPORT_ROW_kVariable(ordinal, text, symbol) symbol(::PPCContext& __restrict, u8*)

extern "C" {
void
#include "export_table_pre.inc"
#include "xboxkrnl/export_table.inc"
#include "xam/export_table.inc"
#include "xbdm/export_table.inc"
#include "export_table_post.inc"
    RexExportDeclEnd(::PPCContext& __restrict, u8*);
}

#undef REX_EXPORT_ROW_kFunction
#undef REX_EXPORT_ROW_kVariable

#define REX_EXPORT_ROW_kFunction(ordinal, text, symbol) \
  ExportEntry {                                         \
    ordinal, ExportKind::Function, text, &symbol        \
  }
#define REX_EXPORT_ROW_kVariable(ordinal, text, symbol) \
  ExportEntry {                                         \
    ordinal, ExportKind::Variable, text, nullptr        \
  }

namespace rex::runtime {

namespace {

constexpr ExportEntry kXboxkrnl[] = {
#include "export_table_pre.inc"
#include "xboxkrnl/export_table.inc"
#include "export_table_post.inc"
};

constexpr ExportEntry kXAM[] = {
#include "export_table_pre.inc"
#include "xam/export_table.inc"
#include "export_table_post.inc"
};

constexpr ExportEntry kXBDM[] = {
#include "export_table_pre.inc"
#include "xbdm/export_table.inc"
#include "export_table_post.inc"
};

constexpr bool SortedUnique(std::span<const ExportEntry> table) {
  for (size_t i = 1; i < table.size(); ++i) {
    if (table[i - 1].ordinal >= table[i].ordinal) {
      return false;
    }
  }
  return true;
}

static_assert(SortedUnique(kXboxkrnl), "xboxkrnl export table is not sorted by unique ordinal");
static_assert(SortedUnique(kXAM), "Xam export table is not sorted by unique ordinal");
static_assert(SortedUnique(kXBDM), "Xbdm export table is not sorted by unique ordinal");

struct ModuleName {
  ExportModule module;
  std::string_view short_name;
  std::string_view long_name;
};

constexpr ModuleName kModuleNames[] = {
    {ExportModule::Xboxkrnl, "xboxkrnl", "xboxkrnl.exe"},
    {ExportModule::XAM, "xam", "xam.xex"},
    {ExportModule::XBDM, "xbdm", "xbdm.xex"},
};

}  // namespace

std::span<const ExportEntry> Exports(ExportModule module) {
  switch (module) {
    case ExportModule::Xboxkrnl:
      return kXboxkrnl;
    case ExportModule::XAM:
      return kXAM;
    case ExportModule::XBDM:
      return kXBDM;
  }
  return {};
}

const ExportEntry* FindByOrdinal(ExportModule module, u16 ordinal) {
  const auto table = Exports(module);
  const auto it = std::ranges::lower_bound(table, ordinal, {}, &ExportEntry::ordinal);
  return it != table.end() && it->ordinal == ordinal ? &*it : nullptr;
}

std::optional<ExportModule> ModuleFromName(std::string_view name) {
  for (const auto& entry : kModuleNames) {
    const bool is_short = name.size() == entry.short_name.size() &&
                          rex::utf8_starts_with_case(name, entry.short_name);
    if (is_short || rex::utf8_starts_with_case(name, entry.long_name)) {
      return entry.module;
    }
  }
  return std::nullopt;
}

}  // namespace rex::runtime
