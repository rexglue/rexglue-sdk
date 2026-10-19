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
 * Pre-include file for an export table. Defines XE_EXPORT and FLAG.
 */

/**
 * Forwards an export table row to the REX_EXPORT_ROW macro named by its type.
 *
 * @param module The module that owns the export.
 * @param ordinal The number a title import uses to find the export.
 * @param name The export name, passed as text and as the __imp__ symbol.
 * @param type The export kind, kFunction or kVariable.
 *
 * @code
 * XE_EXPORT(xam, 0x00000001, NetDll_WSAStartup, kFunction)
 * @endcode
 */
#define XE_EXPORT(module, ordinal, name, type) REX_EXPORT_ROW_##type(ordinal, #name, __imp__##name)

#define FLAG(t) kXEKernelExportFlag##t
