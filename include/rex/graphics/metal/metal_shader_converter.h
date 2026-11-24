/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_
#define XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_

#include <cstdint>
#include <string>
#include <vector>

// Opaque IR types; the implementation includes the Metal Shader Converter
// headers so callers don't have to.
struct IRRootSignature;

namespace rex {
namespace graphics {
namespace metal {

// Stage 3 of the microcode -> DXBC -> DXIL -> metallib pipeline: compiles a DXIL
// container (from DxbcToDxilConverter) into a Metal library (AIR) using Apple's
// Metal Shader Converter (libmetalirconverter). Produces the bytes a
// MTLLibrary/newLibraryWithData would load; it does not touch a Metal device, so
// it is usable offline.
class MetalShaderConverter {
 public:
  enum class Stage { kVertex, kFragment, kGeometry, kHull, kDomain };

  // Input topology for geometry/tessellation emulation, mirroring IRInputTopology
  // (kept ABI-free in the public header so callers don't include the IR headers).
  enum class InputTopology { kUndefined, kPoint, kLine, kTriangle, kPatch };

  // Result of a geometry-emulation conversion. The Metal geometry path compiles a
  // guest VS (object stage) + a generated GS (mesh stage) into an object+mesh
  // pipeline; building IRRuntimeNewGeometryEmulationPipeline needs the reflection
  // captured here (VS output size, GS max input primitives) plus, for the VS, the
  // synthesized stage-in function.
  struct GeometryEmulationResult {
    bool success = false;
    std::vector<uint8_t> metallib;           // object (VS) or mesh (GS) AIR.
    std::vector<uint8_t> stage_in_metallib;  // VS stage-in AIR (empty for GS).
    std::string function_name;
    uint32_t vertex_output_size_in_bytes = 0;                 // VS reflection.
    uint32_t gs_max_input_primitives_per_mesh_threadgroup = 0;  // GS reflection.
    bool has_mesh_stage = false;
    bool has_geometry_stage = false;
    // Hull (kHull) reflection — the COMPLETE set IRRuntimeTessellationPipelineConfig
    // and IRRuntimeValidateTessellationPipeline consume (port notes §2/§3). Note
    // the config's gsMaxInputPrimitivesPerMeshThreadgroup comes from the DOMAIN
    // stage's ds_max_input_prims_per_mesh_threadgroup, not from any HS field.
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
    // Domain (kDomain) reflection.
    bool has_domain_info = false;
    uint32_t ds_max_input_prims_per_mesh_threadgroup = 0;
    uint32_t ds_input_control_point_count = 0;
    uint32_t ds_input_control_point_size = 0;
    uint32_t ds_patch_constants_size = 0;
    uint32_t ds_tessellator_domain = 0;
    bool ds_tessellation_type_half = false;
    std::string error;
  };

  MetalShaderConverter() = default;
  ~MetalShaderConverter();

  // Builds the shared Xbox-360 root signature. Must succeed before Convert().
  bool Initialize(bool bindless_resources_used, std::string* error_out = nullptr);

  bool is_available() const { return root_signature_ != nullptr; }

  // Compiles a DXIL container to a metallib for the given stage. Returns false
  // and fills error_out on failure.
  bool Convert(Stage stage, const std::vector<uint8_t>& dxil,
               std::vector<uint8_t>& metallib_out,
               std::string* error_out = nullptr);

  // Compiles a DXIL container with geometry+tessellation emulation enabled and
  // captures the reflection needed to build a geometry- or tessellation-emulation
  // pipeline. Used by the point/rectangle/quad primitive-expansion path (guest VS
  // as object stage, generated GS as mesh stage) and by the tessellation leg
  // (prebuilt tess VS as object stage, prebuilt HS, guest VS compiled as the
  // domain stage). stage must be kVertex, kGeometry, kHull or kDomain; only
  // kVertex synthesizes (and requires) a stage-in function.
  bool ConvertForGeometryEmulation(Stage stage,
                                   const std::vector<uint8_t>& dxil,
                                   InputTopology input_topology,
                                   GeometryEmulationResult& result);

 private:
  // Constructs the root signature mirroring DxbcShaderTranslator::CbufferRegister
  // (b0..b4, space 0) and the D3D12 bindless descriptor-table shape. Static and
  // shader-independent, so one instance is shared across every shader.
  IRRootSignature* CreateXbox360RootSignature(bool bindless_resources_used,
                                              std::string* error_out);

  IRRootSignature* root_signature_ = nullptr;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_
