/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/xsymboliclink.h>

namespace rex::system {

SymbolicLinkObject::SymbolicLinkObject(std::string_view path_in, std::string_view target_in)
    : path(path_in), target(target_in) {}

}  // namespace rex::system
