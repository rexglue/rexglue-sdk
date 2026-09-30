/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

/**
 * Pre-include file for an export table.
 *
 * The includer defines REX_EXPORT_ROW_kFunction and REX_EXPORT_ROW_kVariable, each taking
 * (ordinal, text, symbol), then includes this file, a table file and export_table_post.inc.
 */

#define XE_EXPORT(module, ordinal, name, type) REX_EXPORT_ROW_##type(ordinal, #name, __imp__##name)

#define FLAG(t) kXEKernelExportFlag##t
