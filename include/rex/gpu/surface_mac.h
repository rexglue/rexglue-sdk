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
  // Takes ownership of `metal_view` (an SDL_MetalView) and destroys it on
  // teardown. `layer` is the CAMetalLayer backing that view. `window` is
  // borrowed and used only to query the current pixel size.
  CAMetalLayerSurface(SDL_Window* window, void* metal_view, const void* layer)
      : window_(window), metal_view_(metal_view), layer_(layer) {}
  ~CAMetalLayerSurface() override;

  TypeIndex GetType() const override { return kTypeIndex_CAMetalLayer; }
  const void* layer() const { return layer_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
  void* metal_view_;
  const void* layer_;
};

}  // namespace gpu
}  // namespace rex
