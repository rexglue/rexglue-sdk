/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

/**
 * Pre-include file for an ordinal table.
 * Use this to build tables of constants describing the ordinals:
 *
 * // Build an ordinal enum to make it easy to lookup ordinals.
 * #include "rex/kernel/util/ordinal_table_pre.inc"
 * namespace ordinals {
 * enum {
 *   #include "xenia/kernel/my_module/my_module_table.inc"
 * };
 * }  // namespace ordinals
 * #include "rex/kernel/util/ordinal_table_post.inc"
 */

/**
 * Defines an enumerator named for the export with the ordinal as its value.
 *
 * @param module The module that owns the export.
 * @param ordinal The export ordinal.
 * @param name The export name, used as the enumerator name.
 * @param type The export kind, ignored here.
 *
 * @code
 * XE_EXPORT(xboxkrnl, 0x0000000C, ExConsoleGameRegion, kVariable)
 * @endcode
 */
#define XE_EXPORT(module, ordinal, name, type) name = ordinal
