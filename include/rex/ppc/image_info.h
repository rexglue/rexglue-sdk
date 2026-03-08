/**
 * @file        rex/ppc/image_info.h
 * @brief       PPC image layout descriptor
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

struct PPCFuncMapping;

namespace rex {

/// PPC image layout passed from the generated config header into ReXApp.
struct PPCImageInfo {
  uint32_t code_base;
  uint32_t code_size;
  uint32_t image_base;
  uint32_t image_size;
  const PPCFuncMapping* func_mappings;
  bool rexcrt_heap = false;  ///< Set by codegen when [rexcrt] has heap functions
};

}  // namespace rex
