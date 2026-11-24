/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_FETCH_CONSTANT_TYPE_H_
#define XENIA_GPU_METAL_METAL_FETCH_CONSTANT_TYPE_H_

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// The shared texture cache, D3D12 and Vulkan serve a texture fetch constant
// whose type field is "invalid texture" (0) when
// --gpu_allow_invalid_fetch_constants is set (default true, upstream
// d36b1b383): titles leave the type bits unset and the hardware fetches
// regardless. Gears of War 3 (4D5308AB, 2026-09-19) issues its occlusion
// queries with such a constant; serving the null texture instead made every
// query count zero and the title cull its whole scene on alternate frames.
// One predicate for every Metal site that gates on the type.
constexpr bool IsTextureFetchConstantServable(xenos::FetchConstantType type,
                                              bool allow_invalid) {
  return type == xenos::FetchConstantType::kTexture ||
         (allow_invalid && type == xenos::FetchConstantType::kInvalidTexture);
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_FETCH_CONSTANT_TYPE_H_
