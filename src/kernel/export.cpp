#include <rex/kernel/export.h>

#include <algorithm>

#include <rex/ppc/context.h>
#include <rex/string/text.h>

#include "kernel/xam/export.h"

#define REX_EXPORT_ROW_kFunction(ordinal, text, symbol) symbol(::PPCContext& __restrict, u8*)
#define REX_EXPORT_ROW_kVariable(ordinal, text, symbol) symbol(::PPCContext& __restrict, u8*)

extern "C" {
void
#include "export_table_pre.inc"
#include "xboxkrnl/export_table.inc"
#include "xbdm/export_table.inc"
#include "export_table_post.inc"
    ReXExportDeclEnd(::PPCContext& __restrict, u8*);
}

#undef REX_EXPORT_ROW_kFunction
#undef REX_EXPORT_ROW_kVariable

#define REX_EXPORT_ROW_kFunction(ordinal, text, symbol) \
  ExportEntry {                                         \
    (ordinal), ExportType::kFunction, (text), &(symbol) \
  }
/**
 * Builds the table entry of a variable export, which has no function pointer.
 *
 * @param ordinal The number a title import uses to find the export.
 * @param text The export name as text.
 * @param symbol The __imp__ symbol, unused here.
 */
#define REX_EXPORT_ROW_kVariable(ordinal, text, symbol) \
  ExportEntry {                                         \
    ordinal, ExportType::kVariable, text, nullptr       \
  }

namespace rex::kernel {

namespace {

constexpr ExportEntry kXboxkrnlExports[] = {
#include "export_table_pre.inc"
#include "xboxkrnl/export_table.inc"
#include "export_table_post.inc"
};

constexpr ExportEntry kXBDMExports[] = {
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

static_assert(SortedUnique(kXboxkrnlExports),
              "xboxkrnl export table is not sorted by unique ordinal");
static_assert(SortedUnique(kXBDMExports), "XBDM export table is not sorted by unique ordinal");

struct ModuleName {
  ExportModule module;
  std::string_view short_name;
  std::string_view long_name;
};

constexpr ModuleName kModuleNames[] = {
    {ExportModule::kXboxkrnl, "xboxkrnl", "xboxkrnl.exe"},
    {ExportModule::kXAM, "xam", "xam.xex"},
    {ExportModule::kXBDM, "xbdm", "xbdm.xex"},
};

struct NoReturnRow {
  ExportModule module;
  u16 ordinal;
};

constexpr NoReturnRow kNoReturnExports[] = {
    {ExportModule::kXboxkrnl, 0x0019},
    {ExportModule::kXboxkrnl, 0x0028},
    {ExportModule::kXboxkrnl, 0x0052},
    {ExportModule::kXboxkrnl, 0x0053},
};

}  // namespace

std::span<const ExportEntry> GetExports(ExportModule module) {
  switch (module) {
    case ExportModule::kXboxkrnl:
      return kXboxkrnlExports;
    case ExportModule::kXAM:
      return GetXAMExports();
    case ExportModule::kXBDM:
      return kXBDMExports;
  }
  return {};
}

const ExportEntry* FindByOrdinal(ExportModule module, u16 ordinal) {
  const auto table = GetExports(module);
  const auto it = std::ranges::lower_bound(table, ordinal, {}, &ExportEntry::ordinal);
  return it != table.end() && it->ordinal == ordinal ? &*it : nullptr;
}

bool IsNoReturnExport(ExportModule module, u16 ordinal) {
  if (module == ExportModule::kXAM) {
    return IsNoReturnXAMExport(ordinal);
  }
  return std::ranges::any_of(kNoReturnExports, [&](const NoReturnRow& row) {
    return row.module == module && row.ordinal == ordinal;
  });
}

std::optional<ExportModule> ModuleFromName(std::string_view name) {
  for (const auto& entry : kModuleNames) {
    const bool is_short = name.size() == entry.short_name.size() &&
                          rex::string::utf8_starts_with_case(name, entry.short_name);
    if (is_short || rex::string::utf8_starts_with_case(name, entry.long_name)) {
      return entry.module;
    }
  }
  return std::nullopt;
}

}  // namespace rex::kernel
