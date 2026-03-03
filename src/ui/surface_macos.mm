/**
 ******************************************************************************
 * ReXGlue SDK — macOS Surface Implementation                                *
 ******************************************************************************
 * Phase 2: CAMetalLayer-backed surface for Vulkan/MoltenVK presentation.     *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/ui/surface_macos.h>

namespace rex {
namespace ui {

bool MacOSMetalSurface::GetSizeImpl(uint32_t& width_out,
                                    uint32_t& height_out) const {
  if (!view_) {
    return false;
  }
  // Use the backing (pixel) size to account for Retina (HiDPI) displays.
  NSSize backing = [view_ convertSizeToBacking:view_.bounds.size];
  width_out = static_cast<uint32_t>(backing.width);
  height_out = static_cast<uint32_t>(backing.height);
  return true;
}

}  // namespace ui
}  // namespace rex
