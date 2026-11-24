/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_METAL_METAL_ALLOC_CENSUS_H_
#define XENIA_UI_METAL_METAL_ALLOC_CENSUS_H_

namespace rex {
namespace ui {
namespace metal {

// XE_METAL_ALLOC_CENSUS=1: accounts every texture, buffer and heap the device
// (an id<MTLDevice>) creates to the code that created it, and logs the live
// bytes per creation site every ten seconds. Does nothing without the
// variable. Diagnostic only: it replaces the device class's creation methods
// for the life of the process.
void MetalAllocCensusInstall(void* device);

}  // namespace metal
}  // namespace ui
}  // namespace rex

#endif  // XENIA_UI_METAL_METAL_ALLOC_CENSUS_H_
