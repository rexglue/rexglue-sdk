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
#define XE_XMA_REGISTER(index, name) static const uint32_t name = index;
#include "runtime/xma/register_table.inc"
#undef XE_XMA_REGISTER
};

struct XmaRegisterInfo {
  const char* name;
};

class XMARegisterFile {
 public:
  static const XmaRegisterInfo* GetRegisterInfo(uint32_t index);

  static const size_t kRegisterCount = (0xFFFF + 1) / 4;
  uint32_t values[kRegisterCount]{};

  uint32_t operator[](uint32_t reg) const { return values[reg]; }
  uint32_t& operator[](uint32_t reg) { return values[reg]; }
};

}  // namespace rex::runtime
