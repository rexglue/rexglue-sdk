/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

std::atomic<int> xam_dialogs_shown_ = {0};

bool xeXamIsUIActive() {
  return xam_dialogs_shown_ > 0;
}

XamModule::XamModule(Runtime* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xam.xex"), loader_data_() {
  RegisterExportTable(export_resolver_);

  // Register all exported functions.
  // #define XE_MODULE_EXPORT_GROUP(m, n) \
//  Register##n##Exports(export_resolver_, kernel_state_);
  // #include <rex/kernel/xam/module_export_groups.inc>
  // #undef XE_MODULE_EXPORT_GROUP
}

std::vector<rex::runtime::Export*> xam_exports(4096);

rex::runtime::Export* RegisterExport_xam(rex::runtime::Export* export_entry) {
  assert_true(export_entry->ordinal < xam_exports.size());
  xam_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}

void XamModule::RegisterExportTable(rex::runtime::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);

// Build the export table used for resolution.
#include "../export_table_pre.inc"
  static rex::runtime::Export xam_export_table[] = {
#include "export_table.inc"
  };
#include "../export_table_post.inc"
  for (size_t i = 0; i < rex::countof(xam_export_table); ++i) {
    auto& export_entry = xam_export_table[i];
    assert_true(export_entry.ordinal < xam_exports.size());
    if (!xam_exports[export_entry.ordinal]) {
      xam_exports[export_entry.ordinal] = &export_entry;
    }
  }
  export_resolver->RegisterTable("xam.xex", &xam_exports);
}

XamModule::~XamModule() {}

}  // namespace xam
}  // namespace kernel
}  // namespace rex
