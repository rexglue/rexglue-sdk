#pragma once
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

#include <rex/system/export_resolver.h>

// Build an ordinal enum to make it easy to lookup ordinals.
#include <rex/system/util/ordinal_table_pre.inc>
namespace ordinals {
enum {
#include <rex/kernel/xam/table.inc>
};
}  // namespace ordinals
#include <rex/system/util/ordinal_table_post.inc>
