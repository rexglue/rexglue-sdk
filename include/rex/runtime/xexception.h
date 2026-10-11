/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @changes     - Extracted exception types from xbox.h
 */

#pragma once

#include <rex/memory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

#pragma pack(push, 4)

/** Registration of a routine the kernel calls when the title terminates. */
struct X_EX_TITLE_TERMINATE_REGISTRATION {
  be_u32 notification_routine; /**< Guest address of the routine. */
  be_u32 priority;             /**< Signed, higher values run first. */
  X_LIST_ENTRY list_entry;
};
REX_STATIC_ASSERT_SIZE(X_EX_TITLE_TERMINATE_REGISTRATION, 16);

/** Guest layout of an exception record. */
struct X_EXCEPTION_RECORD {
  be_u32 code;                      /**< Exception code. */
  be_u32 exception_flags;           /**< Exception flag bits. */
  be_u32 exception_record;          /**< Guest address of the nested exception structure. */
  be_u32 exception_address;         /**< Guest address where the exception occurred. */
  be_u32 number_parameters;         /**< Number of used entries in exception_information. */
  be_u32 exception_information[15]; /**< Exception specific parameters. */
};
REX_STATIC_ASSERT_SIZE(X_EXCEPTION_RECORD, 0x50);

#pragma pack(pop)

}  // namespace rex::runtime
