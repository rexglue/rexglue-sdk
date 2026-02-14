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

#include <cstring>

#include <rex/audio/xma/register_file.h>
#include <rex/math.h>

namespace rex::audio {

XmaRegisterFile::XmaRegisterFile() {
  std::memset(values, 0, sizeof(values));
}

const XmaRegisterInfo* XmaRegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_XMA_REGISTER(index, name)          \
  case index: {                               \
    static const XmaRegisterInfo reg_info = { \
        #name,                                \
    };                                        \
    return &reg_info;                         \
  }
#include <rex/audio/xma/register_table.inc>
#undef XE_XMA_REGISTER
    default:
      return nullptr;
  }
}

}  //  namespace rex::audio
