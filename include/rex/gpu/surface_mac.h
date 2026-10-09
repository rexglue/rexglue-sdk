/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Rien Gupta, 2026 - Adapted for ReXGlue runtime
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/gpu/surface.h>

struct SDL_Window;

namespace rex {
namespace gpu {

/** Presentation target backed by a CAMetalLayer. */
class CAMetalLayerSurface final : public Surface {
 public:
  /**
   * Wraps a CAMetalLayer.
   *
   * @param window The SDL window that owns the layer.
   * @param layer The CAMetalLayer to present to.
   */
  CAMetalLayerSurface(SDL_Window* window, const void* layer) : window_(window), layer_(layer) {}

  TypeIndex GetType() const override { return kTypeIndex_CAMetalLayer; }
  const void* layer() const { return layer_; }

 protected:
  /**
   * Reads the platform surface size.
   *
   * @param width_out Receives the width in physical display pixels.
   * @param height_out Receives the height in physical display pixels.
   * @return True when the size was read.
   */
  bool GetSizeImpl(u32& width_out, u32& height_out) const override;

 private:
  SDL_Window* window_;
  const void* layer_;
};

}  // namespace gpu
}  // namespace rex
