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

#pragma once

#include <string>
#include <string_view>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/export.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** The host state of a symbolic link object. */
struct SymbolicLinkObject {
  /**
   * Constructs a link from the path it is opened by and the path it resolves to.
   *
   * @param path_in The path the link is opened by.
   * @param target_in The path the link resolves to.
   */
  REX_RUNTIME_API SymbolicLinkObject(std::string_view path_in, std::string_view target_in);

  std::string path;   /**< Path titles open the link by. */
  std::string target; /**< Path the link resolves to. */
};

}  // namespace rex::runtime
