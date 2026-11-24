/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_resolve_shader_pipelines.h"

#import <Metal/Metal.h>

#include <rex/logging/log_category.h>

#include "rex/graphics/shaders/bytecode/metal/resolve_dxt3aas1111_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_128bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_16bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_32bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_64bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_8bpp_cs.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {

struct ResolveShaderSource {
  const char* name;
  const uint8_t* metallib;
  size_t metallib_size;
};

#define XE_RESOLVE_SHADER(id) \
  { #id, id##_metallib, sizeof(id##_metallib) }

// Indexed by draw_util::ResolveCopyShaderIndex. The ORDER IS THE CONTRACT:
// the shared selector returns an index into this enum, so a misordered entry
// dispatches the wrong shader for a correct decision. UNSCALED only -- Metal
// is 1x, and selecting a `_scaled` variant would apply resolution-scaled
// addressing to unscaled data.
const ResolveShaderSource kResolveShaders[] = {
    XE_RESOLVE_SHADER(resolve_fast_32bpp_1x2xmsaa_cs),  // kFast32bpp1x2xMSAA
    XE_RESOLVE_SHADER(resolve_fast_32bpp_4xmsaa_cs),    // kFast32bpp4xMSAA
    XE_RESOLVE_SHADER(resolve_fast_64bpp_1x2xmsaa_cs),  // kFast64bpp1x2xMSAA
    XE_RESOLVE_SHADER(resolve_fast_64bpp_4xmsaa_cs),    // kFast64bpp4xMSAA
    XE_RESOLVE_SHADER(resolve_full_8bpp_cs),            // kFull8bpp
    XE_RESOLVE_SHADER(resolve_full_16bpp_cs),           // kFull16bpp
    XE_RESOLVE_SHADER(resolve_full_32bpp_cs),           // kFull32bpp
    XE_RESOLVE_SHADER(resolve_full_64bpp_cs),           // kFull64bpp
    XE_RESOLVE_SHADER(resolve_full_128bpp_cs),          // kFull128bpp
    XE_RESOLVE_SHADER(resolve_dxt3aas1111_cs),          // kDXT3AAs1111
};

#undef XE_RESOLVE_SHADER

static_assert(sizeof(kResolveShaders) / sizeof(kResolveShaders[0]) ==
                  size_t(draw_util::ResolveCopyShaderIndex::kCount),
              "kResolveShaders must have exactly one entry per "
              "ResolveCopyShaderIndex, in enum order");

}  // namespace

MetalResolveShaderPipelines::~MetalResolveShaderPipelines() { Shutdown(); }

bool MetalResolveShaderPipelines::Initialize(void* device) {
  Shutdown();
  if (!device) {
    return false;
  }
  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
  const size_t count =
      sizeof(kResolveShaders) / sizeof(kResolveShaders[0]);
  std::vector<void*> libraries;
  std::vector<void*> pipelines;
  libraries.reserve(count);
  pipelines.reserve(count);
  bool ok = true;
  for (size_t i = 0; i < count && ok; ++i) {
    const ResolveShaderSource& source = kResolveShaders[i];
    // Static storage: wrap without copying, no-op destructor.
    dispatch_data_t data = dispatch_data_create(
        source.metallib, source.metallib_size, nullptr, ^{
        });
    NSError* error = nil;
    id<MTLLibrary> library = [mtl_device newLibraryWithData:data error:&error];
#if !__has_feature(objc_arc)
    dispatch_release(data);
#endif
    if (!library) {
      REXLOG_ERROR(
          "MetalResolveShaderPipelines: newLibraryWithData failed for {}: {}",
          source.name,
          error ? [[error localizedDescription] UTF8String] : "unknown");
      ok = false;
      break;
    }
    // The XeSL MSL entry point is always entry_xe; resolve by name rather
    // than trusting function order.
    id<MTLFunction> function = [library newFunctionWithName:@"entry_xe"];
    if (!function) {
      REXLOG_ERROR("MetalResolveShaderPipelines: entry_xe missing in {}",
             source.name);
      ok = false;
      break;
    }
    id<MTLComputePipelineState> pipeline =
        [mtl_device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
      REXLOG_ERROR("MetalResolveShaderPipelines: pipeline failed for {}: {}",
             source.name,
             error ? [[error localizedDescription] UTF8String] : "unknown");
      ok = false;
      break;
    }
    libraries.push_back((__bridge_retained void*)library);
    pipelines.push_back((__bridge_retained void*)pipeline);
  }
  if (!ok) {
    // All-or-nothing: a partially built table would let a supported index
    // resolve to nothing and silently skip a resolve.
    for (void* library : libraries) {
      CFRelease(library);
    }
    for (void* pipeline : pipelines) {
      CFRelease(pipeline);
    }
    return false;
  }
  libraries_ = std::move(libraries);
  pipelines_ = std::move(pipelines);
  REXLOG_INFO("MetalResolveShaderPipelines: built {} shared resolve-copy pipelines",
         pipelines_.size());
  return true;
}

void MetalResolveShaderPipelines::Shutdown() {
  for (void* pipeline : pipelines_) {
    if (pipeline) {
      CFRelease(pipeline);
    }
  }
  pipelines_.clear();
  for (void* library : libraries_) {
    if (library) {
      CFRelease(library);
    }
  }
  libraries_.clear();
}

void* MetalResolveShaderPipelines::PipelineFor(
    draw_util::ResolveCopyShaderIndex index) const {
  if (pipelines_.empty() ||
      index == draw_util::ResolveCopyShaderIndex::kUnknown) {
    return nullptr;
  }
  size_t slot = size_t(index);
  if (slot >= pipelines_.size()) {
    return nullptr;
  }
  return pipelines_[slot];
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
