#pragma once
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

#include <string>
#include <string_view>

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>

namespace rex::system {

/** The host state of a symbolic link object. */
struct SymbolicLinkObject {
  /**
   * @param path_in The path the link is opened by.
   * @param target_in The path the link resolves to.
   */
  SymbolicLinkObject(std::string_view path_in, std::string_view target_in);

  std::string path;
  std::string target;
};

}  // namespace rex::system
