#include "kernel/xam/export.h"

#include <algorithm>

#include <rex/ppc/context.h>

#define REX_EXPORT_ROW_kFunction(ordinal, text, symbol) symbol(::PPCContext& __restrict, u8*)
#define REX_EXPORT_ROW_kVariable(ordinal, text, symbol) symbol(::PPCContext& __restrict, u8*)

extern "C" {
void
#include "kernel/export_table_pre.inc"
#include "kernel/xam/export_table.inc"
#include "kernel/export_table_post.inc"
    ReXExportDeclEnd(::PPCContext& __restrict, u8*);
}

#undef REX_EXPORT_ROW_kFunction
#undef REX_EXPORT_ROW_kVariable

#define REX_EXPORT_ROW_kFunction(ordinal, text, symbol) \
  ExportEntry {                                         \
    (ordinal), ExportType::kFunction, (text), &(symbol) \
  }
/** Builds the entry of a variable export, which has no function pointer. */
#define REX_EXPORT_ROW_kVariable(ordinal, text, symbol) \
  ExportEntry {                                         \
    ordinal, ExportType::kVariable, text, nullptr       \
  }

namespace rex::kernel {

namespace {

constexpr ExportEntry kExports[] = {
#include "kernel/export_table_pre.inc"
#include "kernel/xam/export_table.inc"
#include "kernel/export_table_post.inc"
};

constexpr bool SortedUnique(std::span<const ExportEntry> table) {
  for (size_t i = 1; i < table.size(); ++i) {
    if (table[i - 1].ordinal >= table[i].ordinal) {
      return false;
    }
  }
  return true;
}

static_assert(SortedUnique(kExports), "XAM export table is not sorted by unique ordinal");

constexpr u16 kNoReturnExports[] = {0x01A4, 0x01A5, 0x01A9};

}  // namespace

std::span<const ExportEntry> GetXAMExports() {
  return kExports;
}

bool IsNoReturnXAMExport(u16 ordinal) {
  return std::ranges::find(kNoReturnExports, ordinal) != std::end(kNoReturnExports);
}

}  // namespace rex::kernel
