/**
 * @file        gpu.h
 * @brief       GPU backend configuration and interfaces
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/gpu/backend.h>
#include <rex/gpu/overlay.h>
#include <rex/gpu/presenter.h>
#include <rex/gpu/surface.h>
#include <rex/gpu/ui_thread.h>
#include <rex/types.h>

namespace rex::gpu {

/** Structural configuration for the GPU backend module. */
struct GPUConfig {
  /** Backend host API, "any", "d3d12", "vulkan" or "null". */
  std::string backend = "any";
  /** D3D12 adapter index. -1 picks the first hardware adapter, -2 picks WARP. */
  i32 d3d12_adapter = -1;
  /** Vulkan physical device index. -1 picks automatically. */
  i32 vulkan_device = -1;
  /** Default of gpu.resolution_scale_x, 1 to 8. */
  i32 resolution_scale_x = 1;
  /** Default of gpu.resolution_scale_y, 1 to 8. */
  i32 resolution_scale_y = 1;
};

}  // namespace rex::gpu
