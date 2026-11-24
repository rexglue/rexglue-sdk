/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <fmt/format.h>
#include "rex/graphics/metal/metal_shader.h"

#import <Metal/Metal.h>

#include <rex/logging/log_category.h>

namespace rex {
namespace graphics {
namespace metal {

MetalShader::MetalTranslation::~MetalTranslation() {
  if (metal_function_) {
    id<MTLFunction> function = (__bridge_transfer id<MTLFunction>)metal_function_;
    (void)function;
    metal_function_ = nullptr;
  }
  if (metal_library_) {
    id<MTLLibrary> library = (__bridge_transfer id<MTLLibrary>)metal_library_;
    (void)library;
    metal_library_ = nullptr;
  }
}

bool MetalShader::MetalTranslation::CreateMetalFunction(
    void* device, const std::vector<uint8_t>& metallib) {
  if (!device || metallib.empty()) {
    return false;
  }
  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;

  // XE_METAL_DUMP_METALLIB_DIR=<dir>: every converted shader as
  // <dir>/<vs|ps>_<ucode hash>_<modification>.metallib, for offline AIR
  // inspection (xcrun metal-objdump) of what the translator + converter emit.
  static const char* const s_dump_dir = std::getenv("XE_METAL_DUMP_METALLIB_DIR");
  if (s_dump_dir && s_dump_dir[0]) {
    const bool is_ps = GetShader().GetType() == xenos::ShaderType::kPixel;
    std::string path =
        fmt::format("{}/{}_{:016X}_{:X}.metallib", s_dump_dir,
                    is_ps ? "ps" : "vs", GetShader().GetUcodeDataHash(),
                    GetModification());
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(metallib.data(), 1, metallib.size(), f);
      fclose(f);
    }
  }

  // Wrap the metallib bytes without copying; dispatch_data_create keeps them
  // alive until the library is built.
  dispatch_data_t data = dispatch_data_create(
      metallib.data(), metallib.size(), dispatch_get_main_queue(),
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* error = nil;
  id<MTLLibrary> library = [mtl_device newLibraryWithData:data error:&error];
#if !__has_feature(objc_arc)
  dispatch_release(data);
#endif
  if (!library) {
    REXLOG_ERROR("MetalShader: newLibraryWithData failed: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return false;
  }

  // Metal Shader Converter emits a single entry point per metallib; take the
  // first (its name is MSC-assigned, e.g. from the DXIL entry).
  NSArray<NSString*>* names = [library functionNames];
  if (![names count]) {
    REXLOG_ERROR("MetalShader: metallib has no functions");
    return false;
  }
  id<MTLFunction> function = [library newFunctionWithName:[names firstObject]];
  if (!function) {
    REXLOG_ERROR("MetalShader: newFunctionWithName failed");
    return false;
  }

  metal_library_ = (__bridge_retained void*)library;
  metal_function_ = (__bridge_retained void*)function;
  return true;
}

MetalShader::MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                         const uint32_t* ucode_dwords, size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : DXBCShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                 ucode_source_endian) {}

std::unique_ptr<::rex::gpu::Shader::Translation> MetalShader::CreateTranslationInstance(
    uint64_t modification) {
  return std::make_unique<MetalTranslation>(*this, modification);
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
