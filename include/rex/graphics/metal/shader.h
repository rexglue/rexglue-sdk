/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/metal/metal_shader_converter.h>

#ifdef __OBJC__
@protocol MTLLibrary;
@protocol MTLFunction;
@protocol MTLDevice;
#endif

namespace rex::graphics::metal {

class DxbcToDxilConverter;
class MetalShaderConverter;

class MetalShader : public DxbcShader {
 public:
  MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
              const uint32_t* ucode_dwords, size_t ucode_dword_count);
  ~MetalShader() override;

#ifdef __OBJC__
  id<MTLLibrary> library() const { return library_; }
  id<MTLFunction> function() const { return function_; }

  void SetMetalLibrary(id<MTLLibrary> library, id<MTLFunction> function) {
    library_ = library;
    function_ = function;
    is_valid_ = (function != nil);
  }

  // Full shader translation pipeline:
  //   1. Caller provides DXBC bytecode (from DxbcShaderTranslator)
  //   2. DXBC → DXIL (via DxbcToDxilConverter)
  //   3. DXIL → Metal IR (via MetalShaderConverter / Apple MSC)
  //   4. Metal IR → MTLLibrary (via newLibraryWithData:)
  // Returns true if the shader is now valid and ready for pipeline creation.
  bool TranslateToMetal(id<MTLDevice> device,
                        const std::vector<uint8_t>& dxbc_data,
                        DxbcToDxilConverter& dxbc_converter,
                        MetalShaderConverter& metal_converter);
#endif

  bool is_valid() const { return is_valid_; }
  void set_valid(bool valid) { is_valid_ = valid; }

  // Intermediate data accessors (for debugging / cache).
  const std::vector<uint8_t>& dxil_data() const { return dxil_data_; }
  const std::vector<uint8_t>& metallib_data() const { return metallib_data_; }
  const std::string& metal_function_name() const {
    return metal_function_name_;
  }
  const MetalShaderReflectionInfo& reflection_info() const {
    return reflection_info_;
  }

  // Release intermediate DXIL and metallib data to free memory.
  // Call after the shader has been successfully translated and cached.
  // The MTLLibrary/MTLFunction remain valid — the GPU has its own compiled
  // representation, so the host-side intermediate bytes are not needed.
  void ClearIntermediateData() {
    dxil_data_.clear();
    dxil_data_.shrink_to_fit();
    metallib_data_.clear();
    metallib_data_.shrink_to_fit();
  }

 private:
#ifdef __OBJC__
  id<MTLLibrary> library_ = nil;
  id<MTLFunction> function_ = nil;
#endif
  bool is_valid_ = false;

  // Intermediate shader data retained for caching and debugging.
  std::vector<uint8_t> dxil_data_;
  std::vector<uint8_t> metallib_data_;
  std::string metal_function_name_;
  MetalShaderReflectionInfo reflection_info_;
};

}  // namespace rex::graphics::metal
