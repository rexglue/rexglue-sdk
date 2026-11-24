/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_XENOS_SAMPLE_POSITIONS_H_
#define XENIA_GPU_METAL_METAL_XENOS_SAMPLE_POSITIONS_H_

#import <Metal/Metal.h>

#include <cstdint>
#include <cstdlib>
#include <initializer_list>

#include <rex/logging/log_category.h>
#include "rex/ui/metal/metal_provider.h"

namespace rex {
namespace graphics {
namespace metal {

// Xenos rasterizes 2x and 4x MSAA at fixed positions (the R6xx/R7xx table,
// Canary cc4981a46 kXenosSamplePositions2x/4x), in 1/16 pixel from the pixel
// center, by GUEST sample:
//   2x: {(-4, 4), (4, -4)}
//   4x: {(-2, -2), (2, 2), (-6, 6), (6, -6)}
// UE3 titles shift their viewport by the guest sample 0 offset under
// predicated tiling to line restored depth up with that sample, so host
// samples at other positions can fail the depth test against restored depth.
//
// Metal positions are by HOST sample, in [0, 1) from the top-left corner.
// Host sample h holds guest sample xe_metal_sample_to_guest(h)
// (metal_edram_msl.h): 2x swaps 0 and 1, 4x swaps 1 and 2. So each host
// sample gets the Xenos position of the guest sample it stores.
inline constexpr MTLSamplePosition kMetalXenosSamplePositions2x[2] = {
    {0.75f, 0.25f},  // host 0 = guest 1 (4, -4)
    {0.25f, 0.75f},  // host 1 = guest 0 (-4, 4)
};
inline constexpr MTLSamplePosition kMetalXenosSamplePositions4x[4] = {
    {0.375f, 0.375f},  // host 0 = guest 0 (-2, -2)
    {0.125f, 0.875f},  // host 1 = guest 2 (-6, 6)
    {0.625f, 0.625f},  // host 2 = guest 1 (2, 2)
    {0.875f, 0.125f},  // host 3 = guest 3 (6, -6)
};

// Every render pass on a 2x or 4x surface rasterizes at the Xenos positions
// (adopt-xenos-msaa-sample-positions, DEFAULT ON since 2026-10-09: the Gears
// of War cell-door slot regains its shading pass; corpus diffs are edge
// coverage only; perf-neutral on USF4/LEGO). XE_METAL_XENOS_SAMPLE_POSITIONS=0
// is the exact rollback (positions never set).
inline bool MetalXenosSamplePositionsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XE_METAL_XENOS_SAMPLE_POSITIONS");
    return !(value && value[0] == '0' && !value[1]);
  }();
  return enabled;
}

// Sets the Xenos positions on a pass whose attachments are multisampled, when
// enabled and supported. Every render encoder is created through this, so all
// passes touching a surface agree on its positions. Returns the descriptor.
inline MTLRenderPassDescriptor* MetalApplyXenosSamplePositions(
    MTLRenderPassDescriptor* pass) {
  if (!pass || !MetalXenosSamplePositionsEnabled()) {
    return pass;
  }
  NSUInteger sample_count = pass.defaultRasterSampleCount;
  for (NSUInteger i = 0; i < 8; ++i) {
    id<MTLTexture> texture = pass.colorAttachments[i].texture;
    if (texture && texture.sampleCount > sample_count) {
      sample_count = texture.sampleCount;
    }
  }
  for (id<MTLTexture> texture :
       {pass.depthAttachment.texture, pass.stencilAttachment.texture}) {
    if (texture && texture.sampleCount > sample_count) {
      sample_count = texture.sampleCount;
    }
  }
  if (sample_count != 2 && sample_count != 4) {
    return pass;
  }
  static const bool supported = [] {
    id<MTLDevice> device =
        MTLCreateSystemDefaultDevice();  // a capability query: the same family of device the provider picks
    bool result = device && device.areProgrammableSamplePositionsSupported;
    if (!result) {
      REXLOG_WARN(
          "Metal: programmable sample positions unsupported; "
          "XE_METAL_XENOS_SAMPLE_POSITIONS keeps the standard positions");
    }
    return result;
  }();
  if (!supported) {
    return pass;
  }
  if (sample_count == 2) {
    [pass setSamplePositions:kMetalXenosSamplePositions2x count:2];
  } else {
    [pass setSamplePositions:kMetalXenosSamplePositions4x count:4];
  }
  return pass;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_XENOS_SAMPLE_POSITIONS_H_
