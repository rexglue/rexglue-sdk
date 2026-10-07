/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - recompilation framework
 *
 * @changes     - Extracted exception types from xbox.h
 */

#pragma once

#include <rex/memory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

#pragma pack(push, 4)

struct X_EX_TITLE_TERMINATE_REGISTRATION {
  be_u32 notification_routine;  // 0x0
  be_u32 priority;              // 0x4
  X_LIST_ENTRY list_entry;      // 0x8
};
REX_STATIC_ASSERT_SIZE(X_EX_TITLE_TERMINATE_REGISTRATION, 16);

// https://msdn.microsoft.com/en-us/library/windows/desktop/aa363082.aspx
struct X_EXCEPTION_RECORD {
  be_u32 code;
  be_u32 exception_flags;
  be_u32 exception_record;
  be_u32 exception_address;
  be_u32 number_parameters;
  be_u32 exception_information[15];
};
REX_STATIC_ASSERT_SIZE(X_EXCEPTION_RECORD, 0x50);

#pragma pack(pop)

}  // namespace rex::runtime
