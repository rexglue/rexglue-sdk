/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 6: Full shader translation pipeline
 */

#import <Metal/Metal.h>

#include <dispatch/dispatch.h>

#include <rex/graphics/metal/shader.h>
#include <rex/graphics/metal/dxbc_to_dxil_converter.h>
#include <rex/graphics/metal/metal_shader_converter.h>
#include <rex/graphics/metal/shader_cache.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

namespace rex::graphics::metal {

MetalShader::MetalShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash,
                         const uint32_t* ucode_dwords,
                         size_t ucode_dword_count)
    : DxbcShader(shader_type, ucode_data_hash, ucode_dwords,
                 ucode_dword_count) {}

MetalShader::~MetalShader() {
#ifdef __OBJC__
  // ARC manages Objective-C object lifetimes.
  function_ = nil;
  library_ = nil;
#endif
}

#ifdef __OBJC__
bool MetalShader::TranslateToMetal(id<MTLDevice> device,
                                   const std::vector<uint8_t>& dxbc_data,
                                   DxbcToDxilConverter& dxbc_converter,
                                   MetalShaderConverter& metal_converter) {
  if (!device) {
    REXGPU_ERROR("MetalShader: No Metal device provided");
    return false;
  }

  if (dxbc_data.empty()) {
    REXGPU_ERROR("MetalShader: No DXBC data available for translation");
    return false;
  }

  // Step 1: Convert DXBC → DXIL.
  std::string dxbc_error;
  if (!dxbc_converter.Convert(dxbc_data, dxil_data_, &dxbc_error)) {
    REXGPU_ERROR("MetalShader: DXBC → DXIL conversion failed: {}", dxbc_error);
    REXGPU_ERROR("  Shader hash: {:016X}", ucode_data_hash());
    REXGPU_ERROR("  DXBC size: {} bytes", dxbc_data.size());
    REXGPU_ERROR("  Shader type: {}", type() == xenos::ShaderType::kVertex ? "vertex" : "pixel");
    REXGPU_ERROR("  Hint: Check if libdxilconv.dylib is compatible");
    REXGPU_ERROR("  Hint: Set REX_DXBC_OUTPUT_DIR=/tmp/rex_shader_dumps to dump failing shaders");
    return false;
  }
  REXGPU_DEBUG("MetalShader: Converted {} bytes DXBC → {} bytes DXIL",
               dxbc_data.size(), dxil_data_.size());

  // Step 2: Convert DXIL → MetalLib via Apple's Metal Shader Converter.
  MetalShaderConversionResult msc_result;
  reflection_info_ = MetalShaderReflectionInfo{};
  bool conversion_ok = false;
  if (type() == xenos::ShaderType::kVertex) {
    conversion_ok = metal_converter.ConvertWithStageEx(
        MetalShaderStage::kVertex, dxil_data_, msc_result, &reflection_info_,
        nullptr, nullptr, false, 0);
  } else {
    conversion_ok = metal_converter.Convert(type(), dxil_data_, msc_result);
  }
  if (!conversion_ok) {
    REXGPU_ERROR("MetalShader: DXIL → Metal conversion failed: {}",
                 msc_result.error_message);
    return false;
  }
  metal_function_name_ = msc_result.function_name;
  metallib_data_ = std::move(msc_result.metallib_data);
  REXGPU_DEBUG("MetalShader: Converted {} bytes DXIL → {} bytes MetalLib",
               dxil_data_.size(), metallib_data_.size());

  // Step 3: Create MTLLibrary from the metallib binary data.
  NSError* error = nil;
  dispatch_data_t data =
      dispatch_data_create(metallib_data_.data(), metallib_data_.size(),
                           nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

  library_ = [device newLibraryWithData:data error:&error];
#if !__has_feature(objc_arc)
  dispatch_release(data);
#endif

  if (!library_) {
    REXGPU_ERROR("MetalShader: Failed to create MTLLibrary: {}",
                 error ? [[error localizedDescription] UTF8String]
                       : "unknown error");
    return false;
  }

  // Step 4: Get the shader entry point function from the library.
  NSString* func_name =
      [NSString stringWithUTF8String:metal_function_name_.c_str()];
  function_ = [library_ newFunctionWithName:func_name];

  if (!function_) {
    // Try alternative function names that MSC may generate.
    NSArray* alt_names = @[ @"main0", @"main", @"vertexMain", @"fragmentMain" ];
    for (NSString* alt in alt_names) {
      function_ = [library_ newFunctionWithName:alt];
      if (function_) {
        metal_function_name_ = [alt UTF8String];
        REXGPU_DEBUG("MetalShader: Found function with alternative name: {}",
                     metal_function_name_);
        break;
      }
    }
  }

  if (!function_) {
    // List available functions for debugging.
    NSArray<NSString*>* names = [library_ functionNames];
    REXGPU_ERROR(
        "MetalShader: Could not find shader function '{}'. "
        "Available functions ({}):",
        metal_function_name_, (unsigned long)[names count]);
    for (NSString* name in names) {
      REXGPU_ERROR("  - {}", [name UTF8String]);
    }
    library_ = nil;
    return false;
  }

  is_valid_ = true;
  REXGPU_DEBUG("MetalShader: Successfully created MTLFunction '{}'",
               metal_function_name_);
  return true;
}
#endif  // __OBJC__

}  // namespace rex::graphics::metal
