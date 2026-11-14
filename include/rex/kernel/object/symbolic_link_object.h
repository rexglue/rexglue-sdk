/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/symbolic_link_object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2015).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/platform.h>

namespace rex::kernel {

struct SymbolicLinkObject {
  /**
   * Constructs a link from the path it is opened by and the path it resolves to.
   *
   * @param path_in The path the link is opened by.
   */
  REX_KERNEL_API SymbolicLinkObject(std::string_view path_in, std::string_view target_in);

  std::string path; /**< Path titles open the link by. */
  std::string target;
};

}  // namespace rex::kernel
