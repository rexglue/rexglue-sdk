/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/xenos/register_file.h>
#include <rex/math.h>

namespace rex::xenos {

const RegisterDesc* RegisterFile::GetRegisterInfo(u32 index) {
  switch (index) {
#define REXGPU_REGISTER(index, type, name) \
  case index: {                            \
    static const RegisterDesc reg_info = { \
        RegisterDesc::Type::type,          \
        #name,                             \
    };                                     \
    return &reg_info;                      \
  }
#include <rex/xenos/register_table.inc>
#undef REXGPU_REGISTER
    default:
      return nullptr;
  }
}

}  // namespace rex::xenos
