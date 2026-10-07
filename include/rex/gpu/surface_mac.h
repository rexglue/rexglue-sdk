#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Rien Gupta, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/gpu/surface.h>

struct SDL_Window;

namespace rex {
namespace gpu {

class CAMetalLayerSurface final : public Surface {
 public:
  CAMetalLayerSurface(SDL_Window* window, const void* layer) : window_(window), layer_(layer) {}

  TypeIndex GetType() const override { return kTypeIndex_CAMetalLayer; }
  const void* layer() const { return layer_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
  const void* layer_;
};

}  // namespace gpu
}  // namespace rex
