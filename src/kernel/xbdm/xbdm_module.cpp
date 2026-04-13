/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>

#include <rex/kernel/xbdm/module.h>
#include <rex/kernel/xbdm/private.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xbdm {
using namespace rex::system;

XbdmModule::XbdmModule(Runtime* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xbdm.xex") {
  RegisterExportTable(export_resolver_);

  // Register all exported functions.
  // #define XE_MODULE_EXPORT_GROUP(m, n) \
//  Register##n##Exports(export_resolver_, kernel_state_);
  // #include <rex/kernel/xbdm/module_export_groups.inc>
  // #undef XE_MODULE_EXPORT_GROUP
}

std::vector<rex::runtime::Export*> xbdm_exports(4096);

rex::runtime::Export* RegisterExport_xbdm(rex::runtime::Export* export_entry) {
  assert_true(export_entry->ordinal < xbdm_exports.size());
  xbdm_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}

void XbdmModule::RegisterExportTable(rex::runtime::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);

// Build the export table used for resolution.
#include "../export_table_pre.inc"
  static rex::runtime::Export xbdm_export_table[] = {
#include "export_table.inc"
  };
#include "../export_table_post.inc"
  for (size_t i = 0; i < rex::countof(xbdm_export_table); ++i) {
    auto& export_entry = xbdm_export_table[i];
    assert_true(export_entry.ordinal < xbdm_exports.size());
    if (!xbdm_exports[export_entry.ordinal]) {
      xbdm_exports[export_entry.ordinal] = &export_entry;
    }
  }
  export_resolver->RegisterTable("xbdm.xex", &xbdm_exports);
}

XbdmModule::~XbdmModule() {}

}  // namespace xbdm
}  // namespace kernel
}  // namespace rex
