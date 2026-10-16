/**
 * @file        gpu/xenos/xenos_graphics.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string_view>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/gpu.h>
#include <rex/result.h>
#include <rex/module.h>

namespace rex::gpu {

class GraphicsSystem;

/** The Xenos graphics service rexgpu-xenos serves to the host API libraries. */
REX_INTERFACE(IXenosGraphics, "rex.gpu.xenos", 1)
 public:
  /**
   * Returns the GPU config the neutral module received at Connect.
   *
   * @return The config, null before Connect.
   */
  virtual const GPUConfig* GetConfig() const = 0;

  /**
   * Claims the host API slot. Only 1 host API library can be loaded.
   *
   * @param module The name of the host API library.
   * @return Ok, or an error naming both libraries when another one holds the slot.
   */
  [[nodiscard]] virtual Result<void> AttachAPI(std::string_view module) = 0;

  /** Releases the host API slot and destroys the graphics system the host API handed over. */
  virtual void DetachAPI() = 0;

  /**
   * Hands over the graphics system of the host API. A null pointer destroys the current one.
   *
   * @param backend The graphics system, the neutral module serves it as the GPU backend.
   * @return Ok, or an error when a graphics system is already set and backend is not null.
   */
  [[nodiscard]] virtual Result<void> SetBackend(std::unique_ptr<GraphicsSystem> backend) = 0;
};

}  // namespace rex::gpu
