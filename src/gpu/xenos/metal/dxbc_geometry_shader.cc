/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include "rex/graphics/metal/dxbc_geometry_shader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "thirdparty/dxbc/DXBCChecksum.h"
#include "rex/assert.h"
#include "rex/math.h"
#include "gpu/xenos/format/dxbc.h"

namespace rex {
namespace graphics {

bool GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    ::rex::gpu::DXBCShaderTranslator::Modification vertex_shader_modification,
    ::rex::gpu::DXBCShaderTranslator::Modification pixel_shader_modification,
    GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  assert_true(vertex_shader_modification.vertex.interpolator_mask ==
              pixel_shader_modification.pixel.interpolator_mask);
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count =
      ::rex::BitCount(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count =
      vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull =
      vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_size;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key_out = key;
  return true;
}

void CreateDxbcGeometryShader(GeometryShaderKey key,
                              std::vector<uint32_t>& shader_out) {
  shader_out.clear();

  // RDEF, ISGN, OSG5, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  shader_out.resize(sizeof(::rex::gpu::dxbc::ContainerHeader) / sizeof(uint32_t) +
                    kBlobCount);
  uint32_t blob_offset_position_dwords =
      sizeof(::rex::gpu::dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(shader_out.size());
  constexpr uint32_t kBlobHeaderSizeDwords =
      sizeof(::rex::gpu::dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // shader_out.resize(rdef_position_dwords);

  // RDEF header - the actual definitions will be written if needed.
  shader_out.resize(rdef_position_dwords +
                    sizeof(::rex::gpu::dxbc::RDEFHeader) / sizeof(uint32_t));
  // Generator name.
  ::rex::gpu::dxbc::AppendAlignedString(shader_out, "Xenia");
  {
    auto& rdef_header = *reinterpret_cast<::rex::gpu::dxbc::RDEFHeader*>(
        shader_out.data() + rdef_position_dwords);
    rdef_header.shader_model = ::rex::gpu::dxbc::RDEFShaderModel::kGeometryShader51;
    rdef_header.compile_flags =
        ::rex::gpu::dxbc::kCompileFlagNoPreshader | ::rex::gpu::dxbc::kCompileFlagPreferFlowControl |
        ::rex::gpu::dxbc::kCompileFlagIEEEStrictness | ::rex::gpu::dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(::rex::gpu::dxbc::RDEFHeader);
    rdef_header.fourcc = ::rex::gpu::dxbc::RDEFHeader::FourCC::k51;
    rdef_header.InitializeSizes();
  }

  uint32_t system_cbuffer_size_vector_aligned_bytes = 0;

  if (key.type == PipelineGeometryShader::kPointList) {
    // Need point parameters from the system constants.

    // Constant types - float2 only.
    // Names.
    name_ptr =
        uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_float2 = name_ptr;
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "float2");
    // Types.
    uint32_t rdef_type_float2_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_type_float2_ptr =
        uint32_t((rdef_type_float2_position_dwords - rdef_position_dwords) *
                 sizeof(uint32_t));
    shader_out.resize(rdef_type_float2_position_dwords +
                      sizeof(::rex::gpu::dxbc::RDEFType) / sizeof(uint32_t));
    {
      auto& rdef_type_float2 = *reinterpret_cast<::rex::gpu::dxbc::RDEFType*>(
          shader_out.data() + rdef_type_float2_position_dwords);
      rdef_type_float2.variable_class = ::rex::gpu::dxbc::RDEFVariableClass::kVector;
      rdef_type_float2.variable_type = ::rex::gpu::dxbc::RDEFVariableType::kFloat;
      rdef_type_float2.row_count = 1;
      rdef_type_float2.column_count = 2;
      rdef_type_float2.name_ptr = rdef_name_ptr_float2;
    }

    // Constants:
    // - float2 xe_point_constant_diameter
    // - float2 xe_point_screen_diameter_to_ndc_radius
    enum PointConstant : uint32_t {
      kPointConstantConstantDiameter,
      kPointConstantScreenDiameterToNDCRadius,
      kPointConstantCount,
    };
    // Names.
    name_ptr =
        uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_point_constant_diameter = name_ptr;
    name_ptr +=
        ::rex::gpu::dxbc::AppendAlignedString(shader_out, "xe_point_constant_diameter");
    uint32_t rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius = name_ptr;
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(
        shader_out, "xe_point_screen_diameter_to_ndc_radius");
    // Constants.
    uint32_t rdef_constants_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_constants_ptr =
        uint32_t((rdef_constants_position_dwords - rdef_position_dwords) *
                 sizeof(uint32_t));
    shader_out.resize(rdef_constants_position_dwords +
                      sizeof(::rex::gpu::dxbc::RDEFVariable) / sizeof(uint32_t) *
                          kPointConstantCount);
    {
      auto rdef_constants = reinterpret_cast<::rex::gpu::dxbc::RDEFVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      // float2 xe_point_constant_diameter
      static_assert(
          sizeof(::rex::gpu::DXBCShaderTranslator::SystemConstants ::
                     point_constant_diameter) == sizeof(float) * 2,
          "::rex::gpu::DXBCShaderTranslator point_constant_diameter system constant size "
          "differs between the shader translator and geometry shader "
          "generation");
      static_assert_size(
          ::rex::gpu::DXBCShaderTranslator::SystemConstants::point_constant_diameter,
          sizeof(float) * 2);
      ::rex::gpu::dxbc::RDEFVariable& rdef_constant_point_constant_diameter =
          rdef_constants[kPointConstantConstantDiameter];
      rdef_constant_point_constant_diameter.name_ptr =
          rdef_name_ptr_xe_point_constant_diameter;
      rdef_constant_point_constant_diameter.start_offset_bytes = offsetof(
          ::rex::gpu::DXBCShaderTranslator::SystemConstants, point_constant_diameter);
      rdef_constant_point_constant_diameter.size_bytes = sizeof(float) * 2;
      rdef_constant_point_constant_diameter.flags = ::rex::gpu::dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_constant_diameter.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_constant_diameter.start_texture = UINT32_MAX;
      rdef_constant_point_constant_diameter.start_sampler = UINT32_MAX;
      // float2 xe_point_screen_diameter_to_ndc_radius
      static_assert(
          sizeof(::rex::gpu::DXBCShaderTranslator::SystemConstants ::
                     point_screen_diameter_to_ndc_radius) == sizeof(float) * 2,
          "::rex::gpu::DXBCShaderTranslator point_screen_diameter_to_ndc_radius system "
          "constant size differs between the shader translator and geometry "
          "shader generation");
      ::rex::gpu::dxbc::RDEFVariable& rdef_constant_point_screen_diameter_to_ndc_radius =
          rdef_constants[kPointConstantScreenDiameterToNDCRadius];
      rdef_constant_point_screen_diameter_to_ndc_radius.name_ptr =
          rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_offset_bytes =
          offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                   point_screen_diameter_to_ndc_radius);
      rdef_constant_point_screen_diameter_to_ndc_radius.size_bytes =
          sizeof(float) * 2;
      rdef_constant_point_screen_diameter_to_ndc_radius.flags =
          ::rex::gpu::dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_screen_diameter_to_ndc_radius.type_ptr =
          rdef_type_float2_ptr;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_texture =
          UINT32_MAX;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_sampler =
          UINT32_MAX;
    }

    // Constant buffers - xe_system_cbuffer only.

    // Names.
    name_ptr =
        uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_system_cbuffer = name_ptr;
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "xe_system_cbuffer");
    // Constant buffers.
    uint32_t rdef_cbuffer_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_cbuffer_position_dwords +
                      sizeof(::rex::gpu::dxbc::RDEFCbuffer) / sizeof(uint32_t));
    {
      auto& rdef_cbuffer_system = *reinterpret_cast<::rex::gpu::dxbc::RDEFCbuffer*>(
          shader_out.data() + rdef_cbuffer_position_dwords);
      rdef_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_cbuffer_system.variable_count = kPointConstantCount;
      rdef_cbuffer_system.variables_ptr = rdef_constants_ptr;
      auto rdef_constants = reinterpret_cast<const ::rex::gpu::dxbc::RDEFVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      for (uint32_t i = 0; i < kPointConstantCount; ++i) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     rdef_constants[i].start_offset_bytes +
                         rdef_constants[i].size_bytes);
      }
      system_cbuffer_size_vector_aligned_bytes =
          rex::align(system_cbuffer_size_vector_aligned_bytes,
                    uint32_t(sizeof(uint32_t) * 4));
      rdef_cbuffer_system.size_vector_aligned_bytes =
          system_cbuffer_size_vector_aligned_bytes;
    }

    // Bindings - xe_system_cbuffer only.
    uint32_t rdef_binding_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_binding_position_dwords +
                      sizeof(::rex::gpu::dxbc::RDEFInputBind) / sizeof(uint32_t));
    {
      auto& rdef_binding_cbuffer_system =
          *reinterpret_cast<::rex::gpu::dxbc::RDEFInputBind*>(shader_out.data() +
                                                  rdef_binding_position_dwords);
      rdef_binding_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_binding_cbuffer_system.type = ::rex::gpu::dxbc::RDEFInputType::kCbuffer;
      rdef_binding_cbuffer_system.bind_point =
          uint32_t(::rex::gpu::DXBCShaderTranslator::CbufferRegister::kSystemConstants);
      rdef_binding_cbuffer_system.bind_count = 1;
      rdef_binding_cbuffer_system.flags = ::rex::gpu::dxbc::kRDEFInputFlagUserPacked;
    }

    // Pointers in the header.
    {
      auto& rdef_header = *reinterpret_cast<::rex::gpu::dxbc::RDEFHeader*>(
          shader_out.data() + rdef_position_dwords);
      rdef_header.cbuffer_count = 1;
      rdef_header.cbuffers_ptr =
          uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) *
                   sizeof(uint32_t));
      rdef_header.input_bind_count = 1;
      rdef_header.input_binds_ptr =
          uint32_t((rdef_binding_position_dwords - rdef_position_dwords) *
                   sizeof(uint32_t));
    }
  }

  {
    auto& blob_header = *reinterpret_cast<::rex::gpu::dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = ::rex::gpu::dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input signature
  // ***************************************************************************

  // Clip and cull distances are tightly packed together into registers, but
  // have separate signature parameters with each being a vec4-aligned window.
  uint32_t input_clip_distance_count =
      key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t input_cull_distance_count =
      (key.user_clip_plane_cull ? key.user_clip_plane_count : 0) +
      key.has_vertex_kill_and;
  uint32_t input_clip_and_cull_distance_count =
      input_clip_distance_count + input_cull_distance_count;

  // Interpolators, position, clip and cull distances (parameters containing
  // only clip or cull distances, and also one parameter containing both if
  // present), point size.
  uint32_t isgn_parameter_count =
      key.interpolator_count + 1 +
      ((input_clip_and_cull_distance_count + 3) / 4) +
      uint32_t(input_cull_distance_count &&
               (input_clip_distance_count & 3) != 0) +
      key.has_point_size;

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(isgn_position_dwords +
                    sizeof(::rex::gpu::dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(::rex::gpu::dxbc::SignatureParameter) / sizeof(uint32_t) *
                        isgn_parameter_count);

  // Names (after the parameters).
  name_ptr =
      uint32_t((shader_out.size() - isgn_position_dwords) * sizeof(uint32_t));
  uint32_t isgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t isgn_name_ptr_sv_position = name_ptr;
  name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t isgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }
  uint32_t isgn_name_ptr_sv_cull_distance = name_ptr;
  if (input_cull_distance_count) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "SV_CullDistance");
  }
  uint32_t isgn_name_ptr_xepsize = name_ptr;
  if (key.has_point_size) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "XEPSIZE");
  }

  // Header and parameters.
  uint32_t input_register_interpolators = UINT32_MAX;
  uint32_t input_register_position;
  uint32_t input_register_clip_and_cull_distances = UINT32_MAX;
  uint32_t input_register_point_size = UINT32_MAX;
  {
    // Header.
    auto& isgn_header = *reinterpret_cast<::rex::gpu::dxbc::Signature*>(
        shader_out.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(::rex::gpu::dxbc::Signature);

    // Parameters.
    auto isgn_parameters = reinterpret_cast<::rex::gpu::dxbc::SignatureParameter*>(
        shader_out.data() + isgn_position_dwords +
        sizeof(::rex::gpu::dxbc::Signature) / sizeof(uint32_t));
    uint32_t isgn_parameter_index = 0;
    uint32_t input_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      input_register_interpolators = input_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(isgn_parameter_index < isgn_parameter_count);
        ::rex::gpu::dxbc::SignatureParameter& isgn_interpolator =
            isgn_parameters[isgn_parameter_index++];
        isgn_interpolator.semantic_name_ptr = isgn_name_ptr_texcoord;
        isgn_interpolator.semantic_index = i;
        isgn_interpolator.component_type =
            ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
        isgn_interpolator.register_index = input_register_index++;
        isgn_interpolator.mask = 0b1111;
        isgn_interpolator.always_reads_mask = 0b1111;
      }
    }

    // Position (SV_Position).
    input_register_position = input_register_index;
    assert_true(isgn_parameter_index < isgn_parameter_count);
    ::rex::gpu::dxbc::SignatureParameter& isgn_sv_position =
        isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_name_ptr_sv_position;
    isgn_sv_position.system_value = ::rex::gpu::dxbc::SystemValueName::kPosition;
    isgn_sv_position.component_type =
        ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = input_register_index++;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b1111;

    // Clip and cull distances (SV_ClipDistance#, SV_CullDistance#).
    if (input_clip_and_cull_distance_count) {
      input_register_clip_and_cull_distances = input_register_index;
      uint32_t isgn_cull_distance_semantic_index = 0;
      for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
        if (i < input_clip_distance_count) {
          ::rex::gpu::dxbc::SignatureParameter& isgn_sv_clip_distance =
              isgn_parameters[isgn_parameter_index++];
          isgn_sv_clip_distance.semantic_name_ptr =
              isgn_name_ptr_sv_clip_distance;
          isgn_sv_clip_distance.semantic_index = i / 4;
          isgn_sv_clip_distance.system_value = ::rex::gpu::dxbc::SystemValueName::kClipDistance;
          isgn_sv_clip_distance.component_type =
              ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_clip_distance.register_index = input_register_index;
          uint8_t isgn_sv_clip_distance_mask =
              (UINT8_C(1) << std::min(input_clip_distance_count - i,
                                      UINT32_C(4))) -
              1;
          isgn_sv_clip_distance.mask = isgn_sv_clip_distance_mask;
          isgn_sv_clip_distance.always_reads_mask = isgn_sv_clip_distance_mask;
        }
        if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
          ::rex::gpu::dxbc::SignatureParameter& isgn_sv_cull_distance =
              isgn_parameters[isgn_parameter_index++];
          isgn_sv_cull_distance.semantic_name_ptr =
              isgn_name_ptr_sv_cull_distance;
          isgn_sv_cull_distance.semantic_index =
              isgn_cull_distance_semantic_index++;
          isgn_sv_cull_distance.system_value = ::rex::gpu::dxbc::SystemValueName::kCullDistance;
          isgn_sv_cull_distance.component_type =
              ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_cull_distance.register_index = input_register_index;
          uint8_t isgn_sv_cull_distance_mask =
              (UINT8_C(1) << std::min(input_clip_and_cull_distance_count - i,
                                      UINT32_C(4))) -
              1;
          if (i < input_clip_distance_count) {
            isgn_sv_cull_distance_mask &=
                ~((UINT8_C(1) << (input_clip_distance_count - i)) - 1);
          }
          isgn_sv_cull_distance.mask = isgn_sv_cull_distance_mask;
          isgn_sv_cull_distance.always_reads_mask = isgn_sv_cull_distance_mask;
        }
        ++input_register_index;
      }
    }

    // Point size (XEPSIZE).
    if (key.has_point_size) {
      input_register_point_size = input_register_index;
      assert_true(isgn_parameter_index < isgn_parameter_count);
      ::rex::gpu::dxbc::SignatureParameter& isgn_point_size =
          isgn_parameters[isgn_parameter_index++];
      isgn_point_size.semantic_name_ptr = isgn_name_ptr_xepsize;
      isgn_point_size.component_type =
          ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
      isgn_point_size.register_index = input_register_index++;
      isgn_point_size.mask = 0b0001;
      isgn_point_size.always_reads_mask =
          key.type == PipelineGeometryShader::kPointList ? 0b0001 : 0;
    }

    assert_true(isgn_parameter_index == isgn_parameter_count);
  }

  {
    auto& blob_header = *reinterpret_cast<::rex::gpu::dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = ::rex::gpu::dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Output signature
  // ***************************************************************************

  // Interpolators, point coordinates, position, clip distances.
  uint32_t osgn_parameter_count = key.interpolator_count +
                                  key.has_point_coordinates + 1 +
                                  ((input_clip_distance_count + 3) / 4);

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(osgn_position_dwords +
                    sizeof(::rex::gpu::dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(::rex::gpu::dxbc::SignatureParameterForGS) / sizeof(uint32_t) *
                        osgn_parameter_count);

  // Names (after the parameters).
  name_ptr =
      uint32_t((shader_out.size() - osgn_position_dwords) * sizeof(uint32_t));
  uint32_t osgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t osgn_name_ptr_xespritetexcoord = name_ptr;
  if (key.has_point_coordinates) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "XESPRITETEXCOORD");
  }
  uint32_t osgn_name_ptr_sv_position = name_ptr;
  name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t osgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += ::rex::gpu::dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }

  // Header and parameters.
  uint32_t output_register_interpolators = UINT32_MAX;
  uint32_t output_register_point_coordinates = UINT32_MAX;
  uint32_t output_register_position;
  uint32_t output_register_clip_distances = UINT32_MAX;
  {
    // Header.
    auto& osgn_header = *reinterpret_cast<::rex::gpu::dxbc::Signature*>(
        shader_out.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(::rex::gpu::dxbc::Signature);

    // Parameters.
    auto osgn_parameters = reinterpret_cast<::rex::gpu::dxbc::SignatureParameterForGS*>(
        shader_out.data() + osgn_position_dwords +
        sizeof(::rex::gpu::dxbc::Signature) / sizeof(uint32_t));
    uint32_t osgn_parameter_index = 0;
    uint32_t output_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      output_register_interpolators = output_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(osgn_parameter_index < osgn_parameter_count);
        ::rex::gpu::dxbc::SignatureParameterForGS& osgn_interpolator =
            osgn_parameters[osgn_parameter_index++];
        osgn_interpolator.semantic_name_ptr = osgn_name_ptr_texcoord;
        osgn_interpolator.semantic_index = i;
        osgn_interpolator.component_type =
            ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_interpolator.register_index = output_register_index++;
        osgn_interpolator.mask = 0b1111;
      }
    }

    // Point coordinates (XESPRITETEXCOORD).
    if (key.has_point_coordinates) {
      output_register_point_coordinates = output_register_index;
      assert_true(osgn_parameter_index < osgn_parameter_count);
      ::rex::gpu::dxbc::SignatureParameterForGS& osgn_point_coordinates =
          osgn_parameters[osgn_parameter_index++];
      osgn_point_coordinates.semantic_name_ptr = osgn_name_ptr_xespritetexcoord;
      osgn_point_coordinates.component_type =
          ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_point_coordinates.register_index = output_register_index++;
      osgn_point_coordinates.mask = 0b0011;
      osgn_point_coordinates.never_writes_mask = 0b1100;
    }

    // Position (SV_Position).
    output_register_position = output_register_index;
    assert_true(osgn_parameter_index < osgn_parameter_count);
    ::rex::gpu::dxbc::SignatureParameterForGS& osgn_sv_position =
        osgn_parameters[osgn_parameter_index++];
    osgn_sv_position.semantic_name_ptr = osgn_name_ptr_sv_position;
    osgn_sv_position.system_value = ::rex::gpu::dxbc::SystemValueName::kPosition;
    osgn_sv_position.component_type =
        ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
    osgn_sv_position.register_index = output_register_index++;
    osgn_sv_position.mask = 0b1111;

    // Clip distances (SV_ClipDistance#).
    if (input_clip_distance_count) {
      output_register_clip_distances = output_register_index;
      for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
        ::rex::gpu::dxbc::SignatureParameterForGS& osgn_sv_clip_distance =
            osgn_parameters[osgn_parameter_index++];
        osgn_sv_clip_distance.semantic_name_ptr =
            osgn_name_ptr_sv_clip_distance;
        osgn_sv_clip_distance.semantic_index = i / 4;
        osgn_sv_clip_distance.system_value = ::rex::gpu::dxbc::SystemValueName::kClipDistance;
        osgn_sv_clip_distance.component_type =
            ::rex::gpu::dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_sv_clip_distance.register_index = output_register_index++;
        uint8_t osgn_sv_clip_distance_mask =
            (UINT8_C(1) << std::min(input_clip_distance_count - i,
                                    UINT32_C(4))) -
            1;
        osgn_sv_clip_distance.mask = osgn_sv_clip_distance_mask;
        osgn_sv_clip_distance.never_writes_mask =
            osgn_sv_clip_distance_mask ^ 0b1111;
      }
    }

    assert_true(osgn_parameter_index == osgn_parameter_count);
  }

  {
    auto& blob_header = *reinterpret_cast<::rex::gpu::dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = ::rex::gpu::dxbc::BlobHeader::FourCC::kOutputSignatureForGS;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(shex_position_dwords);

  shader_out.push_back(
      ::rex::gpu::dxbc::VersionToken(::rex::gpu::dxbc::ProgramType::kGeometryShader, 5, 1));
  // Reserve space for the length token.
  shader_out.push_back(0);

  ::rex::gpu::dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(::rex::gpu::dxbc::Statistics));
  ::rex::gpu::dxbc::Assembler a(shader_out, stat);

  a.OpDclGlobalFlags(::rex::gpu::dxbc::kGlobalFlagAllResourcesBound);

  if (system_cbuffer_size_vector_aligned_bytes) {
    a.OpDclConstantBuffer(
        ::rex::gpu::dxbc::Src::CB(
            ::rex::gpu::dxbc::Src::kDcl, 0,
            uint32_t(::rex::gpu::DXBCShaderTranslator::CbufferRegister::kSystemConstants),
            uint32_t(::rex::gpu::DXBCShaderTranslator::CbufferRegister::kSystemConstants)),
        system_cbuffer_size_vector_aligned_bytes / (sizeof(uint32_t) * 4));
  }

  ::rex::gpu::dxbc::Primitive input_primitive = ::rex::gpu::dxbc::Primitive::kUndefined;
  uint32_t input_primitive_vertex_count = 0;
  ::rex::gpu::dxbc::PrimitiveTopology output_primitive_topology =
      ::rex::gpu::dxbc::PrimitiveTopology::kUndefined;
  uint32_t max_output_vertex_count = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive = ::rex::gpu::dxbc::Primitive::kPoint;
      input_primitive_vertex_count = 1;
      output_primitive_topology = ::rex::gpu::dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive = ::rex::gpu::dxbc::Primitive::kTriangle;
      input_primitive_vertex_count = 3;
      output_primitive_topology = ::rex::gpu::dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via kLineWithAdjacency to a strip of 2 triangles.
      input_primitive = ::rex::gpu::dxbc::Primitive::kLineWithAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_topology = ::rex::gpu::dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    default:
      std::abort();
  }

  assert_false(key.interpolator_count &&
               input_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclInput(::rex::gpu::dxbc::Dest::V2D(input_primitive_vertex_count,
                                 input_register_interpolators + i));
  }
  a.OpDclInputSIV(
      ::rex::gpu::dxbc::Dest::V2D(input_primitive_vertex_count, input_register_position),
      ::rex::gpu::dxbc::SystemValueName::kPosition);
  // Clip and cull plane declarations are separate in FXC-generated code even
  // for a single register.
  assert_false(input_clip_and_cull_distance_count &&
               input_register_clip_and_cull_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
    if (i < input_clip_distance_count) {
      a.OpDclInput(
          ::rex::gpu::dxbc::Dest::V2D(input_primitive_vertex_count,
                          input_register_clip_and_cull_distances + (i >> 2),
                          (UINT32_C(1) << std::min(
                               input_clip_distance_count - i, UINT32_C(4))) -
                              1));
    }
    if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
      uint32_t cull_distance_mask =
          (UINT32_C(1) << std::min(input_clip_and_cull_distance_count - i,
                                   UINT32_C(4))) -
          1;
      if (i < input_clip_distance_count) {
        cull_distance_mask &=
            ~((UINT32_C(1) << (input_clip_distance_count - i)) - 1);
      }
      a.OpDclInput(
          ::rex::gpu::dxbc::Dest::V2D(input_primitive_vertex_count,
                          input_register_clip_and_cull_distances + (i >> 2),
                          cull_distance_mask));
    }
  }
  if (key.has_point_size && key.type == PipelineGeometryShader::kPointList) {
    assert_true(input_register_point_size != UINT32_MAX);
    a.OpDclInput(::rex::gpu::dxbc::Dest::V2D(input_primitive_vertex_count,
                                 input_register_point_size, 0b0001));
  }

  // At least 1 temporary register needed to discard primitives with NaN
  // position.
  size_t dcl_temps_count_position_dwords = a.OpDclTemps(1);

  a.OpDclInputPrimitive(input_primitive);
  ::rex::gpu::dxbc::Dest stream(::rex::gpu::dxbc::Dest::M(0));
  a.OpDclStream(stream);
  a.OpDclOutputTopology(output_primitive_topology);

  assert_false(key.interpolator_count &&
               output_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclOutput(::rex::gpu::dxbc::Dest::O(output_register_interpolators + i));
  }
  if (key.has_point_coordinates) {
    assert_true(output_register_point_coordinates != UINT32_MAX);
    a.OpDclOutput(::rex::gpu::dxbc::Dest::O(output_register_point_coordinates, 0b0011));
  }
  a.OpDclOutputSIV(::rex::gpu::dxbc::Dest::O(output_register_position),
                   ::rex::gpu::dxbc::SystemValueName::kPosition);
  assert_false(input_clip_distance_count &&
               output_register_clip_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
    a.OpDclOutputSIV(
        ::rex::gpu::dxbc::Dest::O(output_register_clip_distances + (i >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - i,
                                               UINT32_C(4))) -
                          1),
        ::rex::gpu::dxbc::SystemValueName::kClipDistance);
  }

  a.OpDclMaxOutputVertexCount(max_output_vertex_count);

  // Note that after every emit, all o# become initialized and must be written
  // to again.
  // Also, FXC generates only movs (from statically or dynamically indexed
  // v[#][#], from r#, or from a literal) to o# for some reason.
  // emit_then_cut_stream must not be used - it crashes the shader compiler of
  // AMD Software: Adrenalin Edition 23.3.2 on RDNA 3 if it's conditional (after
  // a `retc` or inside an `if`), and it doesn't seem to be generated by FXC or
  // DXC at all.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    a.OpNE(::rex::gpu::dxbc::Dest::R(0), ::rex::gpu::dxbc::Src::V2D(i, input_register_position),
           ::rex::gpu::dxbc::Src::V2D(i, input_register_position));
    a.OpOr(::rex::gpu::dxbc::Dest::R(0, 0b0011), ::rex::gpu::dxbc::Src::R(0, 0b0100),
           ::rex::gpu::dxbc::Src::R(0, 0b1110));
    a.OpOr(::rex::gpu::dxbc::Dest::R(0, 0b0001), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX),
           ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kYYYY));
    a.OpRetC(true, ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX));
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // TODO(Triang3l): For points, handle ps_ucp_mode (transform the host clip
  // space to the guest one, calculate the distances to the user clip planes,
  // cull using the distance from the center for modes 0, 1 and 2, cull and clip
  // per-vertex for modes 2 and 3) - except for the vertex kill flag.
  if (input_cull_distance_count) {
    for (uint32_t i = 0; i < input_cull_distance_count; ++i) {
      uint32_t cull_distance_register = input_register_clip_and_cull_distances +
                                        ((input_clip_distance_count + i) >> 2);
      uint32_t cull_distance_component = (input_clip_distance_count + i) & 3;
      a.OpLT(::rex::gpu::dxbc::Dest::R(0, 0b0001),
             ::rex::gpu::dxbc::Src::V2D(0, cull_distance_register)
                 .Select(cull_distance_component),
             ::rex::gpu::dxbc::Src::LF(0.0f));
      for (uint32_t j = 1; j < input_primitive_vertex_count; ++j) {
        a.OpLT(::rex::gpu::dxbc::Dest::R(0, 0b0010),
               ::rex::gpu::dxbc::Src::V2D(j, cull_distance_register)
                   .Select(cull_distance_component),
               ::rex::gpu::dxbc::Src::LF(0.0f));
        a.OpAnd(::rex::gpu::dxbc::Dest::R(0, 0b0001), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX),
                ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kYYYY));
      }
      a.OpRetC(true, ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX));
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.
      ::rex::gpu::dxbc::Src point_size_src(::rex::gpu::dxbc::Src::CB(
          0, uint32_t(::rex::gpu::DXBCShaderTranslator::CbufferRegister::kSystemConstants),
          offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                   point_constant_diameter) >>
              4,
          ((offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                     point_constant_diameter[0]) >>
            2) &
           3) |
              (((offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                          point_constant_diameter[1]) >>
                 2) &
                3)
               << 2)));
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        a.OpGE(::rex::gpu::dxbc::Dest::R(0, 0b0001),
               ::rex::gpu::dxbc::Src::V2D(0, input_register_point_size, ::rex::gpu::dxbc::Src::kXXXX),
               ::rex::gpu::dxbc::Src::LF(0.0f));
        a.OpMovC(::rex::gpu::dxbc::Dest::R(0, 0b0011), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX),
                 ::rex::gpu::dxbc::Src::V2D(0, input_register_point_size, ::rex::gpu::dxbc::Src::kXXXX),
                 point_size_src);
        point_size_src = ::rex::gpu::dxbc::Src::R(0, 0b0100);
      }
      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      // XY may contain the point size with the per-vertex override applied, use
      // Z as temporary.
      for (uint32_t i = 0; i < 2; ++i) {
        a.OpLT(::rex::gpu::dxbc::Dest::R(0, 0b0100), ::rex::gpu::dxbc::Src::LF(0.0f),
               point_size_src.SelectFromSwizzled(i));
        a.OpRetC(false, ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kZZZZ));
      }
      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      a.OpMul(
          ::rex::gpu::dxbc::Dest::R(0, 0b0011), point_size_src,
          ::rex::gpu::dxbc::Src::CB(
              0,
              uint32_t(::rex::gpu::DXBCShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                       point_screen_diameter_to_ndc_radius) >>
                  4,
              ((offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                         point_screen_diameter_to_ndc_radius[0]) >>
                2) &
               3) |
                  (((offsetof(::rex::gpu::DXBCShaderTranslator::SystemConstants,
                              point_screen_diameter_to_ndc_radius[1]) >>
                     2) &
                    3)
                   << 2)));
      point_size_src = ::rex::gpu::dxbc::Src::R(0, 0b0100);
      a.OpMul(::rex::gpu::dxbc::Dest::R(0, 0b0011), point_size_src,
              ::rex::gpu::dxbc::Src::V2D(0, input_register_position, ::rex::gpu::dxbc::Src::kWWWW));
      ::rex::gpu::dxbc::Src point_radius_x_src(point_size_src.SelectFromSwizzled(0));
      ::rex::gpu::dxbc::Src point_radius_y_src(point_size_src.SelectFromSwizzled(1));

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_interpolators + j),
                  ::rex::gpu::dxbc::Src::V2D(0, input_register_interpolators + j));
        }
        // Top-left, top-right, bottom-left, bottom-right order (chosen
        // arbitrarily, simply based on clockwise meaning front with
        // FrontCounterClockwise = FALSE, but faceness is ignored for
        // non-polygon primitive types).
        // Bottom is -Y in Direct3D NDC, +V in point sprite coordinates.
        if (key.has_point_coordinates) {
          a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  ::rex::gpu::dxbc::Src::LF(float(i & 1), float(i >> 1), 0.0f, 0.0f));
        }
        // FXC generates only `mov`s for o#, use temporary registers (r0.zw, as
        // r0.xy already used for the point size) for calculations.
        a.OpAdd(::rex::gpu::dxbc::Dest::R(0, 0b0100),
                ::rex::gpu::dxbc::Src::V2D(0, input_register_position, ::rex::gpu::dxbc::Src::kXXXX),
                (i & 1) ? point_radius_x_src : -point_radius_x_src);
        a.OpAdd(::rex::gpu::dxbc::Dest::R(0, 0b1000),
                ::rex::gpu::dxbc::Src::V2D(0, input_register_position, ::rex::gpu::dxbc::Src::kYYYY),
                (i >> 1) ? -point_radius_y_src : point_radius_y_src);
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_position, 0b0011),
                ::rex::gpu::dxbc::Src::R(0, 0b1110));
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_position, 0b1100),
                ::rex::gpu::dxbc::Src::V2D(0, input_register_position));
        // TODO(Triang3l): Handle ps_ucp_mode properly, clip expanded points if
        // needed.
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(
              ::rex::gpu::dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                            (UINT32_C(1) << std::min(
                                 input_clip_distance_count - j, UINT32_C(4))) -
                                1),
              ::rex::gpu::dxbc::Src::V2D(
                  0, input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]
      //
      // Input vertices are implicitly indexable, dcl_indexRange is not needed
      // for the first dimension of a v[#][#] index.

      // Get squares of edge lengths into r0.xyz to choose the longest edge.
      // r0.x = ||12||^2
      a.OpAdd(::rex::gpu::dxbc::Dest::R(0, 0b0011),
              ::rex::gpu::dxbc::Src::V2D(2, input_register_position, 0b0100),
              -::rex::gpu::dxbc::Src::V2D(1, input_register_position, 0b0100));
      a.OpDP2(::rex::gpu::dxbc::Dest::R(0, 0b0001), ::rex::gpu::dxbc::Src::R(0, 0b0100),
              ::rex::gpu::dxbc::Src::R(0, 0b0100));
      // r0.y = ||20||^2
      a.OpAdd(::rex::gpu::dxbc::Dest::R(0, 0b0110),
              ::rex::gpu::dxbc::Src::V2D(0, input_register_position, 0b0100 << 2),
              -::rex::gpu::dxbc::Src::V2D(2, input_register_position, 0b0100 << 2));
      a.OpDP2(::rex::gpu::dxbc::Dest::R(0, 0b0010), ::rex::gpu::dxbc::Src::R(0, 0b1001),
              ::rex::gpu::dxbc::Src::R(0, 0b1001));
      // r0.z = ||01||^2
      a.OpAdd(::rex::gpu::dxbc::Dest::R(0, 0b1100),
              ::rex::gpu::dxbc::Src::V2D(1, input_register_position, 0b0100 << 4),
              -::rex::gpu::dxbc::Src::V2D(0, input_register_position, 0b0100 << 4));
      a.OpDP2(::rex::gpu::dxbc::Dest::R(0, 0b0100), ::rex::gpu::dxbc::Src::R(0, 0b1110),
              ::rex::gpu::dxbc::Src::R(0, 0b1110));

      // Find the longest edge, and select the strip vertex indices into r0.xyz.
      // r0.w = 12 > 20
      a.OpLT(::rex::gpu::dxbc::Dest::R(0, 0b1000), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kYYYY),
             ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX));
      // r0.x = 12 > 01
      a.OpLT(::rex::gpu::dxbc::Dest::R(0, 0b0001), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kZZZZ),
             ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX));
      // r0.x = 12 > 20 && 12 > 01
      a.OpAnd(::rex::gpu::dxbc::Dest::R(0, 0b0001), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kWWWW),
              ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX));
      a.OpIf(true, ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX));
      {
        // 12 is the longest edge, the first triangle in the strip is 012.
        a.OpMov(::rex::gpu::dxbc::Dest::R(0, 0b0111), ::rex::gpu::dxbc::Src::LU(0, 1, 2, 0));
      }
      a.OpElse();
      {
        // r0.x = 20 > 01
        a.OpLT(::rex::gpu::dxbc::Dest::R(0, 0b0001), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kZZZZ),
               ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kYYYY));
        // If 20 is the longest edge, the first triangle in the strip is 120.
        // Otherwise, it's 201.
        a.OpMovC(::rex::gpu::dxbc::Dest::R(0, 0b0111), ::rex::gpu::dxbc::Src::R(0, ::rex::gpu::dxbc::Src::kXXXX),
                 ::rex::gpu::dxbc::Src::LU(1, 2, 0, 0), ::rex::gpu::dxbc::Src::LU(2, 0, 1, 0));
      }
      a.OpEndIf();

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        ::rex::gpu::dxbc::Index input_vertex_index(0, i);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_interpolators + j),
                  ::rex::gpu::dxbc::Src::V2D(input_vertex_index,
                                 input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  ::rex::gpu::dxbc::Src::LF(0.0f));
        }
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_position),
                ::rex::gpu::dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(
              ::rex::gpu::dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                            (UINT32_C(1) << std::min(
                                 input_clip_distance_count - j, UINT32_C(4))) -
                                1),
              ::rex::gpu::dxbc::Src::V2D(
                  input_vertex_index,
                  input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }

      // Construct the fourth vertex using r1 as temporary storage, including
      // for the final operation as FXC generates only `mov`s for o#.
      stat.temp_register_count =
          std::max(UINT32_C(2), stat.temp_register_count);
      for (uint32_t j = 0; j < key.interpolator_count; ++j) {
        uint32_t input_register_interpolator = input_register_interpolators + j;
        a.OpAdd(::rex::gpu::dxbc::Dest::R(1),
                -::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 0), input_register_interpolator),
                ::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 1), input_register_interpolator));
        a.OpAdd(::rex::gpu::dxbc::Dest::R(1), ::rex::gpu::dxbc::Src::R(1),
                ::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 2), input_register_interpolator));
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_interpolators + j),
                ::rex::gpu::dxbc::Src::R(1));
      }
      if (key.has_point_coordinates) {
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                ::rex::gpu::dxbc::Src::LF(0.0f));
      }
      a.OpAdd(::rex::gpu::dxbc::Dest::R(1),
              -::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 0), input_register_position),
              ::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 1), input_register_position));
      a.OpAdd(::rex::gpu::dxbc::Dest::R(1), ::rex::gpu::dxbc::Src::R(1),
              ::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 2), input_register_position));
      a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_position), ::rex::gpu::dxbc::Src::R(1));
      for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
        uint32_t clip_distance_mask =
            (UINT32_C(1) << std::min(input_clip_distance_count - j,
                                     UINT32_C(4))) -
            1;
        uint32_t input_register_clip_distance =
            input_register_clip_and_cull_distances + (j >> 2);
        a.OpAdd(
            ::rex::gpu::dxbc::Dest::R(1, clip_distance_mask),
            -::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 0), input_register_clip_distance),
            ::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 1), input_register_clip_distance));
        a.OpAdd(
            ::rex::gpu::dxbc::Dest::R(1, clip_distance_mask), ::rex::gpu::dxbc::Src::R(1),
            ::rex::gpu::dxbc::Src::V2D(::rex::gpu::dxbc::Index(0, 2), input_register_clip_distance));
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                              clip_distance_mask),
                ::rex::gpu::dxbc::Src::R(1));
      }
      a.OpEmitStream(stream);
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t input_vertex_index = i ^ (i >> 1);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_interpolators + j),
                  ::rex::gpu::dxbc::Src::V2D(input_vertex_index,
                                 input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  ::rex::gpu::dxbc::Src::LF(0.0f));
        }
        a.OpMov(::rex::gpu::dxbc::Dest::O(output_register_position),
                ::rex::gpu::dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(
              ::rex::gpu::dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                            (UINT32_C(1) << std::min(
                                 input_clip_distance_count - j, UINT32_C(4))) -
                                1),
              ::rex::gpu::dxbc::Src::V2D(
                  input_vertex_index,
                  input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    default:
      std::abort();
  }

  a.OpRet();

  // Write the actual number of temporary registers used.
  shader_out[dcl_temps_count_position_dwords] = stat.temp_register_count;

  // Write the shader program length in dwords.
  shader_out[shex_position_dwords + 1] =
      uint32_t(shader_out.size()) - shex_position_dwords;

  {
    auto& blob_header = *reinterpret_cast<::rex::gpu::dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = ::rex::gpu::dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  constexpr size_t kStatDwords = sizeof(::rex::gpu::dxbc::Statistics) / sizeof(uint32_t);
  static_assert(sizeof(::rex::gpu::dxbc::Statistics) % sizeof(uint32_t) == 0);
  shader_out.resize(stat_position_dwords + kStatDwords);

  std::array<uint32_t, kStatDwords> stat_words{};
  std::memcpy(stat_words.data(), &stat, sizeof(stat));
  std::copy(stat_words.begin(), stat_words.end(),
            shader_out.begin() + stat_position_dwords);

  {
    auto& blob_header = *reinterpret_cast<::rex::gpu::dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = ::rex::gpu::dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t shader_size_bytes = uint32_t(shader_out.size() * sizeof(uint32_t));
  {
    auto& container_header =
        *reinterpret_cast<::rex::gpu::dxbc::ContainerHeader*>(shader_out.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(
        reinterpret_cast<unsigned char*>(shader_out.data()),
        static_cast<unsigned int>(shader_size_bytes),
        reinterpret_cast<unsigned int*>(&container_header.hash));
  }
}

}  // namespace gpu
}  // namespace rex
