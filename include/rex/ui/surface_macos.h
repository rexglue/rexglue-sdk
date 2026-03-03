#pragma once
/**
 ******************************************************************************
 * ReXGlue SDK — macOS Surface                                                *
 ******************************************************************************
 * Phase 2: Native Cocoa windowing surface backed by CAMetalLayer.            *
 * Used for Vulkan presentation via MoltenVK (VK_EXT_metal_surface).          *
 ******************************************************************************
 */

#include <rex/ui/surface.h>

// Forward-declare the Objective-C types we store as opaque pointers so this
// header can be included from plain C++ translation units that do NOT import
// <Cocoa/Cocoa.h>.
#ifdef __OBJC__
@class NSView;
@class CAMetalLayer;
#else
typedef void NSView;
typedef void CAMetalLayer;
#endif

namespace rex {
namespace ui {

class MacOSMetalSurface final : public Surface {
 public:
  explicit MacOSMetalSurface(NSView* view, CAMetalLayer* metal_layer)
      : view_(view), metal_layer_(metal_layer) {}

  TypeIndex GetType() const override { return kTypeIndex_MacOSMetalLayer; }

  NSView* view() const { return view_; }
  CAMetalLayer* metal_layer() const { return metal_layer_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  NSView* view_;
  CAMetalLayer* metal_layer_;
};

}  // namespace ui
}  // namespace rex
