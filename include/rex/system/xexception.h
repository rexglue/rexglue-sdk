/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - Xbox 360 recompilation framework
 *
 * @changes     - Extracted exception types from xbox.h
 */

#pragma once

#include <rex/memory.h>
#include <rex/system/xtypes.h>

namespace rex::system {

#pragma pack(push, 4)

struct X_EX_TITLE_TERMINATE_REGISTRATION {
  be<uint32_t> notification_routine;  // 0x0
  be<uint32_t> priority;              // 0x4
  X_LIST_ENTRY list_entry;            // 0x8
};
static_assert_size(X_EX_TITLE_TERMINATE_REGISTRATION, 16);

// https://msdn.microsoft.com/en-us/library/windows/desktop/aa363082.aspx
struct X_EXCEPTION_RECORD {
  be<uint32_t> code;
  be<uint32_t> exception_flags;
  be<uint32_t> exception_record;
  be<uint32_t> exception_address;
  be<uint32_t> number_parameters;
  be<uint32_t> exception_information[15];
};
static_assert_size(X_EXCEPTION_RECORD, 0x50);

#pragma pack(pop)

}  // namespace rex::system
