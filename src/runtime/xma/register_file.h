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

#pragma once

#include <cstdint>
#include <cstdlib>

namespace rex::runtime {

struct XMARegister {
#define XE_XMA_REGISTER(index, name) static const u32 name = index;
#include "runtime/xma/register_table.inc"

#include <rex/types.h>
#undef XE_XMA_REGISTER
};

struct XmaRegisterInfo {
  const char* name;
};

class XMARegisterFile {
 public:
  static const XmaRegisterInfo* GetRegisterInfo(u32 index);

  static const size_t kRegisterCount = (0xFFFF + 1) / 4;
  u32 values[kRegisterCount]{};

  u32 operator[](u32 reg) const { return values[reg]; }
  u32& operator[](u32 reg) { return values[reg]; }
};

}  // namespace rex::runtime
