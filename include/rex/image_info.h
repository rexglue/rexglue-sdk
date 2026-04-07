/**
 * @file        image_info.h
 * @brief       Image layout descriptor for recompiled binaries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

struct PPCFuncMapping;

namespace rex {

/// PPC image layout passed from the generated config header into ReXApp.
struct PPCImageInfo {
  u32 code_base;
  u32 code_size;
  u32 image_base;
  u32 image_size;
  const PPCFuncMapping* func_mappings;
  bool rexcrt_heap = false;  ///< Set by codegen when [rexcrt] has heap functions
};

}  // namespace rex
