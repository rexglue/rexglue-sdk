/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/xenos/registers.h>

namespace rex::xenos {
namespace reg {

const Register RB_COLOR_INFO::rt_register_indices[4] = {
    xenos::Register::RB_COLOR_INFO,
    xenos::Register::RB_COLOR1_INFO,
    xenos::Register::RB_COLOR2_INFO,
    xenos::Register::RB_COLOR3_INFO,
};

const Register RB_BLENDCONTROL::rt_register_indices[4] = {
    xenos::Register::RB_BLENDCONTROL0,
    xenos::Register::RB_BLENDCONTROL1,
    xenos::Register::RB_BLENDCONTROL2,
    xenos::Register::RB_BLENDCONTROL3,
};

}  // namespace reg
}  // namespace rex::xenos
