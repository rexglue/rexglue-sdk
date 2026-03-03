/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Static Recompilation Runtime
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 *
 * Metal Shader Converter (MSC) wrapper for the Metal shader pipeline.
 *
 * Converts DXIL bytecode to Metal IR (metallib) using Apple's Metal Shader
 * Converter library (libmetalirconverter.dylib).
 *
 * Pipeline: Xenos ucode → DXBC → DXIL → [this] → Metal IR → MTLLibrary
 *
 * Key features:
 *   - Xbox 360 root signature matching Xenia's resource layout
 *   - Geometry shader emulation via MSC mesh shaders
 *   - Tessellation pipeline emulation
 *   - Shader reflection for vertex inputs, function constants, etc.
 *   - Minimum GPU family / deployment target configuration
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

struct IRVersionedInputLayoutDescriptor;

// Shader stage for Metal conversion.
enum class MetalShaderStage {
  kVertex,
  kFragment,
  kGeometry,
  kCompute,
  kHull,
  kDomain,
};

// Result of shader conversion.
struct MetalShaderConversionResult {
  bool success = false;
  std::vector<uint8_t> metallib_data;
  std::string error_message;
  std::string function_name;
  bool has_mesh_stage = false;
  bool has_geometry_stage = false;
};

// Reflection data for a vertex input attribute.
struct MetalShaderReflectionInput {
  std::string name;
  uint8_t attribute_index = 0;
};

// Reflection data for a function constant.
struct MetalShaderFunctionConstant {
  std::string name;
  uint32_t type = 0;
};

// Full shader reflection info extracted after conversion.
struct MetalShaderReflectionInfo {
  uint32_t vertex_output_size_in_bytes = 0;
  uint32_t vertex_input_count = 0;
  std::vector<MetalShaderReflectionInput> vertex_inputs;
  uint32_t gs_max_input_primitives_per_mesh_threadgroup = 0;
  std::vector<MetalShaderFunctionConstant> function_constants;

  // Hull shader info.
  bool has_hull_info = false;
  uint32_t hs_max_patches_per_object_threadgroup = 0;
  uint32_t hs_max_object_threads_per_patch = 0;
  uint32_t hs_patch_constants_size = 0;
  uint32_t hs_input_control_point_count = 0;
  uint32_t hs_output_control_point_count = 0;
  uint32_t hs_output_control_point_size = 0;
  uint32_t hs_tessellator_domain = 0;
  uint32_t hs_tessellator_partitioning = 0;
  uint32_t hs_tessellator_output_primitive = 0;
  bool hs_tessellation_type_half = false;
  float hs_max_tessellation_factor = 0.0f;

  // Domain shader info.
  bool has_domain_info = false;
  uint32_t ds_max_input_prims_per_mesh_threadgroup = 0;
  uint32_t ds_input_control_point_count = 0;
  uint32_t ds_input_control_point_size = 0;
  uint32_t ds_patch_constants_size = 0;
  uint32_t ds_tessellator_domain = 0;
  bool ds_tessellation_type_half = false;
};

// Converts DXIL shaders to Metal IR using Apple's Metal Shader Converter.
// Uses Xbox 360-specific root signatures matching Xenia's resource layout.
class MetalShaderConverter {
 public:
  MetalShaderConverter();
  ~MetalShaderConverter();
  MetalShaderConverter(const MetalShaderConverter&) = delete;
  MetalShaderConverter& operator=(const MetalShaderConverter&) = delete;

  // Initialize the converter (dynamically loads libmetalirconverter).
  bool Initialize();

  // Check if the converter is available.
  bool IsAvailable() const { return is_available_; }

  // Convert DXIL to Metal IR using the shader type to determine stage.
  bool Convert(xenos::ShaderType shader_type,
               const std::vector<uint8_t>& dxil_data,
               MetalShaderConversionResult& result);

  // Convert with explicit stage specification.
  bool ConvertWithStage(MetalShaderStage stage,
                        const std::vector<uint8_t>& dxil_data,
                        MetalShaderConversionResult& result);

  // Convert with full options including reflection and geometry emulation.
  bool ConvertWithStageEx(MetalShaderStage stage,
                          const std::vector<uint8_t>& dxil_data,
                          MetalShaderConversionResult& result,
                          MetalShaderReflectionInfo* reflection,
                          const IRVersionedInputLayoutDescriptor* input_layout = nullptr,
                          std::vector<uint8_t>* stage_in_metallib = nullptr,
                          bool enable_geometry_emulation = false,
                          int input_topology = 0);

  // Set minimum GPU family and deployment target for compiled shaders.
  void SetMinimumTarget(uint32_t gpu_family, uint32_t os,
                        const std::string& version);

 private:
  bool is_available_ = false;
  bool has_minimum_target_ = false;
  uint32_t minimum_gpu_family_ = 0;
  uint32_t minimum_os_ = 0;
  std::string minimum_os_version_;

  // Opaque handle to the dynamically loaded MSC library.
  void* msc_lib_ = nullptr;

  // Function pointers for MSC API (loaded dynamically).
  // These are void* to avoid requiring the metal_irconverter.h header
  // in the public interface. Cast to proper types in the .mm file.
  struct MSCFunctions {
    void* IRCompilerCreate = nullptr;
    void* IRCompilerDestroy = nullptr;
    void* IRCompilerAllocCompileAndLink = nullptr;
    void* IRCompilerSetGlobalRootSignature = nullptr;
    void* IRCompilerSetCompatibilityFlags = nullptr;
    void* IRCompilerSetInputTopology = nullptr;
    void* IRCompilerEnableGeometryAndTessellationEmulation = nullptr;
    void* IRCompilerIgnoreRootSignature = nullptr;
    void* IRCompilerSetFunctionConstantResourceSpace = nullptr;
    void* IRCompilerSetMinimumGPUFamily = nullptr;
    void* IRCompilerSetMinimumDeploymentTarget = nullptr;
    void* IRObjectCreateFromDXIL = nullptr;
    void* IRObjectDestroy = nullptr;
    void* IRObjectGetMetalLibBinary = nullptr;
    void* IRObjectGetReflection = nullptr;
    void* IRMetalLibBinaryCreate = nullptr;
    void* IRMetalLibBinaryDestroy = nullptr;
    void* IRMetalLibGetBytecodeSize = nullptr;
    void* IRMetalLibGetBytecode = nullptr;
    void* IRMetalLibSynthesizeStageInFunction = nullptr;
    void* IRRootSignatureCreateFromDescriptor = nullptr;
    void* IRRootSignatureDestroy = nullptr;
    void* IRErrorGetPayload = nullptr;
    void* IRErrorDestroy = nullptr;
    void* IRShaderReflectionCreate = nullptr;
    void* IRShaderReflectionDestroy = nullptr;
    void* IRShaderReflectionGetEntryPointFunctionName = nullptr;
    void* IRShaderReflectionCopyVertexInfo = nullptr;
    void* IRShaderReflectionReleaseVertexInfo = nullptr;
    void* IRShaderReflectionCopyGeometryInfo = nullptr;
    void* IRShaderReflectionReleaseGeometryInfo = nullptr;
    void* IRShaderReflectionNeedsFunctionConstants = nullptr;
    void* IRShaderReflectionGetFunctionConstantCount = nullptr;
    void* IRShaderReflectionCopyFunctionConstants = nullptr;
    void* IRShaderReflectionReleaseFunctionConstants = nullptr;
    void* IRShaderReflectionCopyHullInfo = nullptr;
    void* IRShaderReflectionReleaseHullInfo = nullptr;
    void* IRShaderReflectionCopyDomainInfo = nullptr;
    void* IRShaderReflectionReleaseDomainInfo = nullptr;
    void* IRVersionedRootSignatureDescriptorCopyJSONString = nullptr;
    void* IRVersionedRootSignatureDescriptorReleaseString = nullptr;
    void* IRInputLayoutDescriptor1CopyJSONString = nullptr;
    void* IRInputLayoutDescriptor1ReleaseString = nullptr;
    // Optional (MSC 3.0.4+, Feb 2026 package): returns the IRShaderStage that
    // was actually compiled into a metalObject after IRCompilerAllocCompileAndLink.
    // Used for stage auto-detection instead of relying on hard-coded enum values.
    void* IRObjectGetMetalIRShaderStage = nullptr;
  };
  MSCFunctions msc_fn_{};

  // Create Xbox 360 root signature for the given shader stage.
  void* CreateXbox360RootSignature(MetalShaderStage stage,
                                   bool force_all_visibility);

  // Get or create the cached all-visibility root signature.
  void* GetCachedRootSignature();

  // Destroy a root signature.
  void DestroyRootSignature(void* root_sig);

  // Cached root signature (all-visibility, created once).
  void* cached_root_sig_ = nullptr;
};

}  // namespace rex::graphics::metal
