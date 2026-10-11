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

#include "runtime/xma/register_file.h"
#include <rex/math.h>

namespace rex::runtime {

const XMARegisterInfo* XMARegisterFile::GetRegisterInfo(u32 index) {
  switch (index) {
#define REX_XMA_REGISTER(index, name)         \
  case index: {                               \
    static const XMARegisterInfo reg_info = { \
        #name,                                \
    };                                        \
    return &reg_info;                         \
  }
#include "runtime/xma/register_table.inc"
#undef REX_XMA_REGISTER
    default:
      return nullptr;
  }
}

}  //  namespace rex::runtime
