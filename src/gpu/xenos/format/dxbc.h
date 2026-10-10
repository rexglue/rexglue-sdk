/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::gpu::dxbc {

/**
 * Shader model 5_1 byte code generation for Direct3D 12.
 *
 * Only the parts of DXBC the emitters use are present. Any added operation, operand or blob must
 * first be reproduced with FXC to find its dependencies, such as modified STAT fields, and its
 * encoding. DXBC restricts operand usage, so no sequence may be added that FXC would not
 * produce, and where FXC follows another pattern the FXC pattern is used.
 *
 * Absolute, negate and saturate are valid only on instructions that explicitly support them.
 * Component selection in ALU instructions is 0 component for operand types with no data such as
 * samplers and labels, 1 component for scalar destinations and for scalar sources when the
 * destination vector has 1 component masked including scalar immediates, mask for vector
 * destinations, swizzle for vector and scalar sources, replicated for scalars, when the
 * destination has 2 or more components masked with XYZW swizzle for immediates, and select 1 for
 * vector sources when the destination has 1 component masked or is scalar. Resource access and
 * flow control mostly need the mode defined by the instruction. Input operands (v#) are only
 * sources and output operands (o#) are only destinations. Indexable temporaries (x#) are only a
 * destination or a source, not both, of a mov, and such movs count as ArrayInstructions rather
 * than MovInstructions in STAT.
 *
 * FXC ignores the limit of 1 immediate or constant buffer source operand per instruction, accesses
 * x# only with a load and store mov, and moves the inner index operand of nested x# indexing to
 * r# first, so the D3D11.3 Functional Specification is not a reliable reference.
 *
 * Registers written in "if" and not in "else", then used outside unconditionally or under a
 * different condition, must not have uninitialized components. They crash AMD drivers and limit
 * optimization through false dependencies, so l(0, 0, 0, 0) is moved to such components before
 * branching, and PushSystemTemp accepts a zero mask for this purpose.
 *
 * Clamping of non negative values is done first to the lower bound with max, then to the upper
 * bound with min, to match the saturate modifier, which results in 0 for NaN.
 *
 * Pointers in RDEF and signatures are offsets from the start of the blob not including the FourCC
 * and the size, and a 0 pointer is null when applicable. The reflection structures in
 * d3d12shader.h differ from the container layout.
 */
constexpr u8 kAlignmentPadding = 0xAB;

constexpr u32 MakeFourCC(u32 ch0, u32 ch1, u32 ch2, u32 ch3) {
  return ch0 | (ch1 << 8) | (ch2 << 16) | (ch3 << 24);
}

/** Followed by uint32_t[blob_count] byte offsets from the container start to each blob header. */
struct alignas(u32) ContainerHeader {
  static constexpr u32 kFourCC = MakeFourCC('D', 'X', 'B', 'C');
  static constexpr u16 kVersionMajor = 1;
  static constexpr u16 kVersionMinor = 0;
  u32 fourcc;
  /** Checksum of the entire container including this header, computed with this set to 0. */
  u32 hash[4];
  u16 version_major;
  u16 version_minor;
  u32 size_bytes;
  u32 blob_count;
  void InitializeIdentification() {
    fourcc = kFourCC;
    version_major = kVersionMajor;
    version_minor = kVersionMinor;
  }
};
REX_STATIC_ASSERT_SIZE(ContainerHeader, sizeof(u32) * 8);

struct alignas(u32) BlobHeader {
  /** FourCC values in order of appearance in a container. */
  enum class FourCC : u32 {
    kResourceDefinition = MakeFourCC('R', 'D', 'E', 'F'),
    kInputSignature = MakeFourCC('I', 'S', 'G', 'N'),
    kInputSignature_11_1 = MakeFourCC('I', 'S', 'G', '1'),
    kPatchConstantSignature = MakeFourCC('P', 'C', 'S', 'G'),
    kOutputSignature = MakeFourCC('O', 'S', 'G', 'N'),
    kOutputSignatureForGS = MakeFourCC('O', 'S', 'G', '5'),
    kOutputSignature_11_1 = MakeFourCC('O', 'S', 'G', '1'),
    kShaderEx = MakeFourCC('S', 'H', 'E', 'X'),
    kShaderFeatureInfo = MakeFourCC('S', 'F', 'I', '0'),
    kStatistics = MakeFourCC('S', 'T', 'A', 'T'),
  };
  FourCC fourcc;
  u32 size_bytes;
};
REX_STATIC_ASSERT_SIZE(BlobHeader, sizeof(u32) * 2);

/**
 * Appends a string to a DWORD stream.
 *
 * The padding is filled with kAlignmentPadding so equal input gives equal DXBC for driver shader
 * caching.
 *
 * @param dest DWORD stream to append to.
 * @param source Null terminated string.
 * @return Length in bytes aligned to a DWORD.
 */
inline u32 AppendAlignedString(std::vector<u32>& dest, const char* source) {
  size_t size = std::strlen(source) + 1;
  size_t size_aligned = rex::align(size, sizeof(u32));
  size_t dest_position = dest.size();
  dest.resize(dest_position + size_aligned / sizeof(u32));
  std::memcpy(&dest[dest_position], source, size);
  std::memset(reinterpret_cast<u8*>(&dest[dest_position]) + size, dxbc::kAlignmentPadding,
              size_aligned - size);
  return static_cast<u32>(size_aligned);
}

/**
 * Returns the length of a string as if appended to a DWORD stream, in bytes.
 *
 * @param source Null terminated string.
 * @return Length in bytes aligned to a DWORD.
 */
inline u32 GetAlignedStringLength(const char* source) {
  return static_cast<u32>(rex::align(std::strlen(source) + 1, sizeof(u32)));
}

// D3DCOMPILE subset
enum CompileFlags : u32 {
  /** NoPreshader and PreferFlowControl are set by default for shader model 5_1. */
  kCompileFlagNoPreshader = 1 << 8,
  kCompileFlagPreferFlowControl = 1 << 10,
  kCompileFlagIeeeStrictness = 1 << 13,
  kCompileFlagEnableUnboundedDescriptorTables = 1 << 20,
  kCompileFlagAllResourcesBound = 1 << 21,
};

// D3D_SHADER_VARIABLE_CLASS
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class RdefVariableClass : u16 {
  kScalar,
  kVector,
  kMatrixRows,
  kMatrixColumns,
  kObject,
  kStruct,
  kInterfaceClass,
  kInterfacePointer,
};

// D3D_SHADER_VARIABLE_TYPE subset
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class RdefVariableType : u16 {
  kInt = 2,
  kFloat = 3,
  kUInt = 19,
};

// D3D_SHADER_VARIABLE_FLAGS
enum RdefVariableFlags : u8 {
  kRdefVariableFlagUserPacked = 1 << 0,
  kRdefVariableFlagUsed = 1 << 1,
  kRdefVariableFlagInterfacePointer = 1 << 2,
  kRdefVariableFlagInterfaceParameter = 1 << 3,
};

// D3D_SHADER_CBUFFER_FLAGS
enum RdefCbufferFlags : u8 {
  kRdefCbufferFlagUserPacked = 1 << 0,
};

// D3D_CBUFFER_TYPE
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class RdefCbufferType : u32 {
  kCbuffer,
  kTbuffer,
  kInterfacePointers,
  kResourceBindInfo,
};

// D3D_SHADER_INPUT_TYPE
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class RdefInputType : u32 {
  kCbuffer,
  kTbuffer,
  kTexture,
  kSampler,
  kUAVRWTyped,
  kStructured,
  kUAVRWStructured,
  kByteAddress,
  kUAVRWByteAddress,
  kUAVAppendStructured,
  kUAVConsumeStructured,
  kUAVRWStructuredWithCounter,
};

// D3D_RESOURCE_RETURN_TYPE / D3D10_SB_RESOURCE_RETURN_TYPE
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class ResourceReturnType : u32 {
  kVoid,
  kUNorm,
  kSNorm,
  kSInt,
  kUInt,
  kFloat,
  kMixed,
  kDouble,
  kContinued,
};

// D3D12_SRV_DIMENSION / D3D12_UAV_DIMENSION
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class RdefDimension : u32 {
  kUnknown = 0,

  kSRVBuffer = 1,
  kSRVTexture1D,
  kSRVTexture1DArray,
  kSRVTexture2D,
  kSRVTexture2DArray,
  kSRVTexture2DMS,
  kSRVTexture2DMSArray,
  kSRVTexture3D,
  kSRVTextureCube,
  kSRVTextureCubeArray,

  kUAVBuffer = 1,
  kUAVTexture1D,
  kUAVTexture1DArray,
  kUAVTexture2D,
  kUAVTexture2DArray,
  kUAVTexture3D,
};

// D3D_SHADER_INPUT_FLAGS
enum RdefInputFlags : u8 {
  /**
   * For constant buffers, set when declared as `cbuffer` rather than `ConstantBuffer<T>`, which
   * is not dynamically indexable. Differs from kRdefCbufferFlagUserPacked, set in another case.
   */
  kRdefInputFlagUserPacked = 1 << 0,
  kRdefInputFlagComparisonSampler = 1 << 1,
  /** Texture and typed buffer component count minus 1. */
  kRdefInputFlagsComponentsShift = 2,
  kRdefInputFlags2Component = 1 << kRdefInputFlagsComponentsShift,
  kRdefInputFlags3Component = 2 << kRdefInputFlagsComponentsShift,
  kRdefInputFlags4Component = 3 << kRdefInputFlagsComponentsShift,
  kRdefInputFlagUnused = 1 << 4,
};

enum class RdefShaderModel : u32 {
  kPixelShader5_1 = 0xFFFF0501u,
  kVertexShader5_1 = 0xFFFE0501u,
  kGeometryShader5_1 = 0x47530501u,
  kDomainShader5_1 = 0x44530501u,
  kComputeShader5_1 = 0x43530501u,
};

// D3D12_SHADER_TYPE_DESC with some differences.
struct alignas(u32) RdefType {
  RdefVariableClass variable_class;
  RdefVariableType variable_type;
  /** Matrix rows, 1 for other numeric, 0 if not applicable. */
  u16 row_count;
  /** Vector and matrix columns, 1 for other numerics, 0 if not applicable. */
  u16 column_count;
  /** 0 if not an array, except for structures which have 1. */
  u16 element_count;
  /** 0 if not a structure. */
  u16 member_count;
  /** Null if not a structure. */
  u32 members_ptr;
  /** Always 0. */
  u32 unknown_0[4];
  /** A scalar uint is named dword, while uint vectors are named uintN. */
  u32 name_ptr;
};
REX_STATIC_ASSERT_SIZE(RdefType, sizeof(u32) * 9);

struct alignas(u32) RdefStructureMember {
  u32 name_ptr;
  u32 type_ptr;
  u32 offset_bytes;
};
REX_STATIC_ASSERT_SIZE(RdefStructureMember, sizeof(u32) * 3);

/** D3D12_SHADER_VARIABLE_DESC variant, used primarily for constants in constant buffers. */
struct alignas(u32) RdefVariable {
  u32 name_ptr;
  u32 start_offset_bytes;
  u32 size_bytes;
  /** RdefVariableFlags. */
  u32 flags;
  u32 type_ptr;
  u32 default_value_ptr;
  /** UINT32_MAX if no textures used. */
  u32 start_texture;
  /** Number of texture slots possibly used, 0 if no textures used. */
  u32 texture_size;
  /** UINT32_MAX if no textures used. */
  u32 start_sampler;
  /** Number of sampler slots possibly used, 0 if no textures used. */
  u32 sampler_size;
};
REX_STATIC_ASSERT_SIZE(RdefVariable, sizeof(u32) * 10);

/** Sorted by ID. */
struct alignas(u32) RdefCbuffer {
  u32 name_ptr;
  u32 variable_count;
  u32 variables_ptr;
  /** 16 byte aligned. */
  u32 size_vector_aligned_bytes;
  RdefCbufferType type;
  /** RdefCbufferFlags. */
  u32 flags;
};
REX_STATIC_ASSERT_SIZE(RdefCbuffer, sizeof(u32) * 6);

/** D3D12_SHADER_INPUT_BIND_DESC variant, placed in samplers, SRVs, UAVs, CBVs order by ID. */
struct alignas(u32) RdefInputBind {
  u32 name_ptr;
  RdefInputType type;
  ResourceReturnType return_type;
  RdefDimension dimension;
  /**
   * 0 for multisampled textures, whose sample count is in the SRV descriptor, constant buffers,
   * ByteAddressBuffers and samplers. UINT32_MAX for single sampled textures and typed buffers.
   */
  u32 sample_count;
  u32 bind_point;
  /** 0 for unbounded. */
  u32 bind_count;
  /** RdefInputFlags. */
  u32 flags;
  /** Bind point space and ID added in shader model 5_1. */
  u32 bind_point_space;
  u32 id;
};
REX_STATIC_ASSERT_SIZE(RdefInputBind, sizeof(u32) * 10);

struct alignas(u32) RdefHeader {
  enum class FourCC : u32 {
    /** RD11 in Shader Model 5_0 shaders. */
    k5_0 = MakeFourCC('R', 'D', '1', '1'),
    /** RD11 with reversed nibbles in Shader Model 5_0 shaders. */
    k5_1 = 0x25441313u,
  };
  u32 cbuffer_count;
  u32 cbuffers_ptr;
  u32 input_bind_count;
  u32 input_binds_ptr;
  RdefShaderModel shader_model;
  /** CompileFlags. */
  u32 compile_flags;
  u32 generator_name_ptr;
  FourCC fourcc;
  u32 sizeof_header_bytes;
  u32 sizeof_cbuffer_bytes;
  u32 sizeof_input_bind_bytes;
  u32 sizeof_variable_bytes;
  u32 sizeof_type_bytes;
  u32 sizeof_structure_member_bytes;
  /** Always 0. */
  u32 unknown_0;
  void InitializeSizes() {
    sizeof_header_bytes = sizeof(*this);
    sizeof_cbuffer_bytes = sizeof(RdefCbuffer);
    sizeof_input_bind_bytes = sizeof(RdefInputBind);
    sizeof_variable_bytes = sizeof(RdefVariable);
    sizeof_type_bytes = sizeof(RdefType);
    sizeof_structure_member_bytes = sizeof(RdefStructureMember);
  }
};
REX_STATIC_ASSERT_SIZE(RdefHeader, sizeof(u32) * 15);

// D3D_NAME subset
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class Name : u32 {
  kUndefined = 0,
  kPosition = 1,
  kClipDistance = 2,
  kCullDistance = 3,
  kVertexID = 6,
  kIsFrontFace = 9,
  kSampleIndex = 10,
  kFinalQuadEdgeTessFactor = 11,
  kFinalQuadInsideTessFactor = 12,
  kFinalTriEdgeTessFactor = 13,
  kFinalTriInsideTessFactor = 14,
};

// D3D_REGISTER_COMPONENT_TYPE
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class SignatureRegisterComponentType : u32 {
  kUnknown,
  kUInt32,
  kSInt32,
  kFloat32,
};

// D3D_MIN_PRECISION
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class MinPrecision : u32 {
  kDefault,
  kFloat16,
  kFloat2_8,
  kSInt16 = 4,
  kUInt16,
  kAny16 = 0xF0,
  kAny10,
};

// D3D10_INTERNALSHADER_PARAMETER
struct alignas(u32) SignatureParameter {
  u32 semantic_name_ptr;
  u32 semantic_index;
  /** kUndefined for pixel shader outputs, inferred from the component type and shader usage. */
  Name system_value;
  SignatureRegisterComponentType component_type;
  /** o#/v# when there is linkage, SV_Target index or UINT32_MAX in pixel shader output. */
  u32 register_index;
  u8 mask;
  union {
    /** For an output signature. */
    u8 never_writes_mask;
    /** For an input signature. */
    u8 always_reads_mask;
  };
};
REX_STATIC_ASSERT_SIZE(SignatureParameter, sizeof(u32) * 6);

/** D3D11_INTERNALSHADER_PARAMETER_FOR_GS, extends SignatureParameter. */
struct alignas(u32) SignatureParameterForGS {
  /** Stream index, parameters must appear in non decreasing stream order. */
  u32 stream;
  u32 semantic_name_ptr;
  u32 semantic_index;
  Name system_value;
  SignatureRegisterComponentType component_type;
  u32 register_index;
  u8 mask;
  union {
    u8 never_writes_mask;
    u8 always_reads_mask;
  };
};
REX_STATIC_ASSERT_SIZE(SignatureParameterForGS, sizeof(u32) * 7);

/** D3D11_INTERNALSHADER_PARAMETER_11_1, extends SignatureParameterForGS. */
struct alignas(u32) SignatureParameter_11_1 {
  u32 stream;
  u32 semantic_name_ptr;
  u32 semantic_index;
  Name system_value;
  SignatureRegisterComponentType component_type;
  u32 register_index;
  u8 mask;
  union {
    u8 never_writes_mask;
    u8 always_reads_mask;
  };
  MinPrecision min_precision;
};
REX_STATIC_ASSERT_SIZE(SignatureParameter_11_1, sizeof(u32) * 8);

// D3D10_INTERNALSHADER_SIGNATURE
struct alignas(u32) Signature {
  u32 parameter_count;
  /** Points after the header even if the signature is empty. */
  u32 parameter_info_ptr;
};
REX_STATIC_ASSERT_SIZE(Signature, sizeof(u32) * 2);

/** SHADER_FEATURE, low 32 bits. */
enum ShaderFeature0 : u16 {
  kShaderFeature0_Doubles = 1 << 0,
  kShaderFeature0_ComputeShadersPlusRawAndStructuredBuffersViaShader_4_X = 1 << 1,
  kShaderFeature0_UAVsAtEveryStage = 1 << 2,
  kShaderFeature0_64UAVs = 1 << 3,
  kShaderFeature0_MinimumPrecision = 1 << 4,
  kShaderFeature0_11_1_DoubleExtensions = 1 << 5,
  kShaderFeature0_11_1_ShaderExtensions = 1 << 6,
  kShaderFeature0_Level9ComparisonFiltering = 1 << 7,
  kShaderFeature0_TiledResources = 1 << 8,
  kShaderFeature0_StencilRef = 1 << 9,
  kShaderFeature0_InnerCoverage = 1 << 10,
  kShaderFeature0_TypedUAVLoadAdditionalFormats = 1 << 11,
  kShaderFeature0_ROVs = 1 << 12,
  kShaderFeature0_ViewportAndRTArrayIndexFromAnyShaderFeedingRasterizer = 1 << 13,
};

struct alignas(u32) ShaderFeatureInfo {
  /** UINT64 originally, but aligned to 4 rather than 8. */
  u32 feature_flags[2];
};
REX_STATIC_ASSERT_SIZE(ShaderFeatureInfo, sizeof(u32) * 2);

// D3D11_SB_TESSELLATOR_DOMAIN
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class TessellatorDomain : u32 {
  kUndefined,
  kIsoline,
  kTriangle,
  kQuad,
};

// D3D10_SB_PRIMITIVE_TOPOLOGY
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class PrimitiveTopology : u32 {
  kUndefined = 0,
  kPointList = 1,
  kLineList = 2,
  kLineStrip = 3,
  kTriangleList = 4,
  kTriangleStrip = 5,
  kLineListWithAdjacency = 10,
  kLineStripWithAdjacency = 11,
  kTriangleListWithAdjacency = 12,
  kTriangleStripWithAdjacency = 13,
};

// D3D10_SB_PRIMITIVE
// NOLINTNEXTLINE(performance-enum-size): DXBC container field width
enum class Primitive : u32 {
  kUndefined = 0,
  kPoint = 1,
  kLine = 2,
  kTriangle = 3,
  kLineWithAdjacency = 6,
  kTriangleWithAdjacency = 7,
  k1ControlPointPatch = 8,
  k2ControlPointPatch = 9,
  k3ControlPointPatch = 10,
  k4ControlPointPatch = 11,
  k5ControlPointPatch = 12,
  k6ControlPointPatch = 13,
  k7ControlPointPatch = 14,
  k8ControlPointPatch = 15,
  k9ControlPointPatch = 16,
  k10ControlPointPatch = 17,
  k11ControlPointPatch = 18,
  k12ControlPointPatch = 19,
  k13ControlPointPatch = 20,
  k14ControlPointPatch = 21,
  k15ControlPointPatch = 22,
  k16ControlPointPatch = 23,
  k17ControlPointPatch = 24,
  k18ControlPointPatch = 25,
  k19ControlPointPatch = 26,
  k20ControlPointPatch = 27,
  k21ControlPointPatch = 28,
  k22ControlPointPatch = 29,
  k23ControlPointPatch = 30,
  k24ControlPointPatch = 31,
  k25ControlPointPatch = 32,
  k26ControlPointPatch = 33,
  k27ControlPointPatch = 34,
  k28ControlPointPatch = 35,
  k29ControlPointPatch = 36,
  k30ControlPointPatch = 37,
  k31ControlPointPatch = 38,
  k32ControlPointPatch = 39,
};

/** The STAT blob, with the byte offset of each member in its trailing comment. */
struct alignas(u32) Statistics {
  /** Not increased by declarations and labels. */
  u32 instruction_count;   /**< +0 */
  u32 temp_register_count; /**< +4 */
  u32 def_count;           /**< +8 */
  /** Only inputs and outputs, not CBVs, SRVs, UAVs and samplers. */
  u32 dcl_count;               /**< +C */
  u32 float_instruction_count; /**< +10 */
  u32 int_instruction_count;   /**< +14 */
  u32 uint_instruction_count;  /**< +18 */
  /** Counts endif and ret. */
  u32 static_flow_control_count; /**< +1C */
  /** Counts if but not else. */
  u32 dynamic_flow_control_count;    /**< +20 */
  u32 macro_instruction_count;       /**< +24 */
  u32 temp_array_count;              /**< +28 */
  u32 array_instruction_count;       /**< +2C */
  u32 cut_instruction_count;         /**< +30 */
  u32 emit_instruction_count;        /**< +34 */
  u32 texture_normal_instructions;   /**< +38 */
  u32 texture_load_instructions;     /**< +3C */
  u32 texture_comp_instructions;     /**< +40 */
  u32 texture_bias_instructions;     /**< +44 */
  u32 texture_gradient_instructions; /**< +48 */
  /** Not including indexable temp load and store. */
  u32 mov_instruction_count;            /**< +4C */
  u32 movc_instruction_count;           /**< +50 */
  u32 conversion_instruction_count;     /**< +54 */
  u32 unknown_22;                       /**< +58 */
  Primitive input_primitive;            /**< +5C */
  PrimitiveTopology gs_output_topology; /**< +60 */
  u32 gs_max_output_vertex_count;       /**< +64 */
  u32 unknown_26;                       /**< +68 */
  u32 lod_instructions;                 /**< +6C */
  u32 unknown_28;                       /**< +70 */
  u32 unknown_29;                       /**< +74 */
  u32 c_control_points;                 /**< +78 */
  u32 hs_output_primitive;              /**< +7C */
  u32 hs_partitioning;                  /**< +80 */
  TessellatorDomain tessellator_domain; /**< +84 */
  u32 c_barrier_instructions;           /**< +88 */
  u32 c_interlocked_instructions;       /**< +8C */
  u32 c_texture_store_instructions;     /**< +90 */
};
REX_STATIC_ASSERT_SIZE(Statistics, sizeof(u32) * 37);

/**
 * D3D10_SB_TOKENIZED_PROGRAM_TYPE.
 *
 * A shader blob begins with a version token and the shader length in dwords, including the
 * version token and the length token itself.
 */
enum class ProgramType : u8 {
  kPixelShader,
  kVertexShader,
  kGeometryShader,
  kHullShader,
  kDomainShader,
  kComputeShader,
};

constexpr u32 VersionToken(ProgramType program_type, u32 major_version, u32 minor_version) {
  return (static_cast<u32>(program_type) << 16) | (major_version << 4) | minor_version;
}

// D3D10_SB_CUSTOMDATA_CLASS
enum class CustomDataClass : u8 {
  kComment,
  kDebugInfo,
  kOpaque,
  kDclImmediateConstantBuffer,
  kShaderMessage,
  kShaderClipPlaneConstantMappingsForDX9,
};

// D3D10_SB_OPERAND_TYPE subset
enum class OperandType : u8 {
  kTemp = 0,
  kInput = 1,
  kOutput = 2,
  /** Only usable as destination or source, not both, in mov, which makes it an array instruction.
   */
  kIndexableTemp = 3,
  kImmediate32 = 4,
  kSampler = 6,
  kResource = 7,
  kConstantBuffer = 8,
  kLabel = 10,
  kInputPrimitiveID = 11,
  kOutputDepth = 12,
  kNull = 13,
  kOutputCoverageMask = 15,
  kStream = 16,
  kInputControlPoint = 25,
  kInputDomainPoint = 28,
  kUnorderedAccessView = 30,
  kInputThreadID = 32,
  kInputThreadGroupID = 33,
  kInputThreadIDInGroup = 34,
  kInputCoverageMask = 35,
  kOutputDepthLessEqual = 39,
  kOutputStencilRef = 41,
};

// D3D10_SB_OPERAND_NUM_COMPONENTS
enum class OperandDimension : u8 {
  kNoData, /**< D3D10_SB_OPERAND_0_COMPONENT */
  kScalar, /**< D3D10_SB_OPERAND_1_COMPONENT */
  kVector, /**< D3D10_SB_OPERAND_4_COMPONENT */
};

constexpr OperandDimension GetOperandDimension(OperandType type, bool in_dcl = false) {
  switch (type) {
    case OperandType::kSampler:
      return in_dcl ? OperandDimension::kVector : OperandDimension::kNoData;
    case OperandType::kLabel:
    case OperandType::kNull:
    case OperandType::kStream:
      return OperandDimension::kNoData;
    case OperandType::kInputPrimitiveID:
    case OperandType::kOutputDepth:
    case OperandType::kOutputCoverageMask:
    case OperandType::kOutputDepthLessEqual:
    case OperandType::kOutputStencilRef:
      return OperandDimension::kScalar;
    case OperandType::kInputCoverageMask:
      return in_dcl ? OperandDimension::kScalar : OperandDimension::kVector;
    default:
      return OperandDimension::kVector;
  }
}

// D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE
enum class ComponentSelection : u8 {
  kMask,
  kSwizzle,
  kSelect1,
};

struct Index {
  // D3D10_SB_OPERAND_INDEX_REPRESENTATION
  enum class Representation : u8 {
    kImmediate32,
    kImmediate64,
    kRelative,
    kImmediate32PlusRelative,
    kImmediate64PlusRelative,
  };

  u32 index_;
  /**
   * UINT32_MAX if absolute. Lower 2 bits are the component index, upper bits are the temp
   * register index. Applies to indexable temps, inputs, outputs except for pixel shaders,
   * constant buffers and bindings.
   */
  u32 relative_to_temp_;

  Index(u32 index = 0) : index_(index), relative_to_temp_(UINT32_MAX) {}
  Index(u32 temp, u32 temp_component, u32 offset = 0)
      : index_(offset), relative_to_temp_((temp << 2) | temp_component) {}

  Representation GetRepresentation() const {
    if (relative_to_temp_ != UINT32_MAX) {
      return index_ != 0 ? Representation::kImmediate32PlusRelative : Representation::kRelative;
    }
    return Representation::kImmediate32;
  }
  u32 GetLength() const { return relative_to_temp_ != UINT32_MAX ? (index_ != 0 ? 3 : 2) : 1; }
  void Write(std::vector<u32>& code) const {
    if (relative_to_temp_ == UINT32_MAX || index_ != 0) {
      code.push_back(index_);
    }
    if (relative_to_temp_ != UINT32_MAX) {
      code.push_back(std::to_underlying(OperandDimension::kVector) |
                     (std::to_underlying(ComponentSelection::kSelect1) << 2) |
                     ((relative_to_temp_ & 3) << 4) |
                     (std::to_underlying(OperandType::kTemp) << 12) | (1 << 20) |
                     (std::to_underlying(Representation::kImmediate32) << 22));
      code.push_back(relative_to_temp_ >> 2);
    }
  }
};

struct OperandAddress {
  OperandType type_;
  u32 index_dimension_;
  Index index_1d_, index_2d_, index_3d_;

  explicit OperandAddress(OperandType type) : type_(type), index_dimension_(0) {}
  explicit OperandAddress(OperandType type, Index index_1d)
      : type_(type), index_dimension_(1), index_1d_(index_1d) {}
  explicit OperandAddress(OperandType type, Index index_1d, Index index_2d)
      : type_(type), index_dimension_(2), index_1d_(index_1d), index_2d_(index_2d) {}
  explicit OperandAddress(OperandType type, Index index_1d, Index index_2d, Index index_3d)
      : type_(type),
        index_dimension_(3),
        index_1d_(index_1d),
        index_2d_(index_2d),
        index_3d_(index_3d) {}

  OperandDimension GetDimension(bool in_dcl = false) const {
    return GetOperandDimension(type_, in_dcl);
  }
  u32 GetOperandTokenTypeAndIndex() const {
    u32 operand_token = (static_cast<u32>(type_) << 12) | (index_dimension_ << 20);
    if (index_dimension_ > 0) {
      operand_token |= static_cast<u32>(index_1d_.GetRepresentation()) << 22;
      if (index_dimension_ > 1) {
        operand_token |= static_cast<u32>(index_2d_.GetRepresentation()) << 25;
        if (index_dimension_ > 2) {
          operand_token |= static_cast<u32>(index_3d_.GetRepresentation()) << 28;
        }
      }
    }
    return operand_token;
  }
  u32 GetAddressLength() const {
    u32 length = 0;
    if (index_dimension_ > 0) {
      length += index_1d_.GetLength();
      if (index_dimension_ > 1) {
        length += index_2d_.GetLength();
        if (index_dimension_ > 2) {
          length += index_3d_.GetLength();
        }
      }
    }
    return length;
  }
  void Write(std::vector<u32>& code) const {
    if (index_dimension_ > 0) {
      index_1d_.Write(code);
      if (index_dimension_ > 1) {
        index_2d_.Write(code);
        if (index_dimension_ > 2) {
          index_3d_.Write(code);
        }
      }
    }
  }
};

// D3D10_SB_EXTENDED_OPERAND_TYPE
enum class ExtendedOperandType : u8 {
  kEmpty,
  kModifier,
};

// D3D10_SB_OPERAND_MODIFIER
enum class OperandModifier : u8 {
  kNone,
  kNegate,
  kAbsolute,
  kAbsoluteNegate,
};

/**
 * Destination operand.
 *
 * Input destinations (v*) are for declarations only. Vector input declarations use read masks
 * instead of swizzle, while resource declarations use swizzle when they are vector.
 */
struct Dest : OperandAddress {
  /**
   * Ignored for 0 component and 1 component operand types. For 4 component operand types, a
   * write mask of 0 is treated as 0 component.
   */
  u32 write_mask_;

  explicit Dest(OperandType type, u32 write_mask) : OperandAddress(type), write_mask_(write_mask) {}
  explicit Dest(OperandType type, u32 write_mask, Index index_1d)
      : OperandAddress(type, index_1d), write_mask_(write_mask) {}
  explicit Dest(OperandType type, u32 write_mask, Index index_1d, Index index_2d)
      : OperandAddress(type, index_1d, index_2d), write_mask_(write_mask) {}
  explicit Dest(OperandType type, u32 write_mask, Index index_1d, Index index_2d, Index index_3d)
      : OperandAddress(type, index_1d, index_2d, index_3d), write_mask_(write_mask) {}

  static Dest R(u32 index, u32 write_mask = 0b1111) {
    return Dest(OperandType::kTemp, write_mask, index);
  }
  static Dest V1D(u32 index, u32 read_mask = 0b1111) {
    return Dest(OperandType::kInput, read_mask, index);
  }
  static Dest V2D(u32 index_1d, u32 index_2d, u32 read_mask = 0b1111) {
    return Dest(OperandType::kInput, read_mask, index_1d, index_2d);
  }
  static Dest O(Index index, u32 write_mask = 0b1111) {
    return Dest(OperandType::kOutput, write_mask, index);
  }
  static Dest X(u32 index_1d, Index index_2d, u32 write_mask = 0b1111) {
    return Dest(OperandType::kIndexableTemp, write_mask, index_1d, index_2d);
  }
  static Dest VPrim() { return Dest(OperandType::kInputPrimitiveID, 0b0001); }
  static Dest ODepth() { return Dest(OperandType::kOutputDepth, 0b0001); }
  static Dest Null() { return Dest(OperandType::kNull, 0b0000); }
  static Dest OMask() { return Dest(OperandType::kOutputCoverageMask, 0b0001); }
  static Dest M(u32 index) { return Dest(OperandType::kStream, 0b0000, index); }
  static Dest VICP(u32 control_point_count, u32 element, u32 read_mask = 0b1111) {
    return Dest(OperandType::kInputControlPoint, read_mask, control_point_count, element);
  }
  static Dest VDomain(u32 read_mask) { return Dest(OperandType::kInputDomainPoint, read_mask); }
  static Dest U(u32 index_1d, Index index_2d, u32 write_mask = 0b1111) {
    return Dest(OperandType::kUnorderedAccessView, write_mask, index_1d, index_2d);
  }
  static Dest VThreadID(u32 read_mask) { return Dest(OperandType::kInputThreadID, read_mask); }
  static Dest VThreadGroupID(u32 read_mask) {
    return Dest(OperandType::kInputThreadGroupID, read_mask);
  }
  static Dest VThreadIDInGroup(u32 read_mask) {
    return Dest(OperandType::kInputThreadIDInGroup, read_mask);
  }
  static Dest VCoverage() { return Dest(OperandType::kInputCoverageMask, 0b0001); }
  static Dest ODepthLE() { return Dest(OperandType::kOutputDepthLessEqual, 0b0001); }
  static Dest OStencilRef() { return Dest(OperandType::kOutputStencilRef, 0b0001); }

  u32 GetMask(bool in_dcl = false) const {
    OperandDimension dimension = GetDimension(in_dcl);
    switch (dimension) {
      case OperandDimension::kNoData:
        return 0b0000;
      case OperandDimension::kScalar:
        return 0b0001;
      case OperandDimension::kVector:
        return write_mask_;
      default:
        REX_UNHANDLED_CASE(dimension);
        return 0b0000;
    }
  }
  [[nodiscard]] Dest Mask(u32 write_mask) const {
    Dest new_dest(*this);
    new_dest.write_mask_ = write_mask;
    return new_dest;
  }
  [[nodiscard]] Dest MaskMasked(u32 write_mask) const {
    Dest new_dest(*this);
    new_dest.write_mask_ &= write_mask;
    return new_dest;
  }
  static u32 GetMaskSingleComponent(u32 write_mask) {
    u32 component;
    if (rex::bit_scan_forward(write_mask, &component)) {
      if ((write_mask >> component) == 1) {
        return component;
      }
    }
    return UINT32_MAX;
  }
  u32 GetMaskSingleComponent(bool in_dcl = false) const {
    return GetMaskSingleComponent(GetMask(in_dcl));
  }

  u32 GetLength() const { return 1 + OperandAddress::GetAddressLength(); }
  void Write(std::vector<u32>& code, bool in_dcl = false) const {
    u32 operand_token = GetOperandTokenTypeAndIndex();
    OperandDimension dimension = GetDimension(in_dcl);
    if (dimension == OperandDimension::kVector) {
      if (write_mask_) {
        REX_ASSERT(write_mask_ <= 0b1111);
        operand_token |= (std::to_underlying(ComponentSelection::kMask) << 2) | (write_mask_ << 4);
      } else {
        dimension = OperandDimension::kNoData;
      }
    }
    operand_token |= static_cast<u32>(dimension);
    code.push_back(operand_token);
    OperandAddress::Write(code);
  }
};

struct Src : OperandAddress {
  enum : u8 {
    kXYZW = 0b11100100,
    kXXXX = 0b00000000,
    kYYYY = 0b01010101,
    kZZZZ = 0b10101010,
    kWWWW = 0b11111111,
  };

  /** Ignored for 0 component and 1 component operand types. */
  u32 swizzle_;
  bool absolute_ = false;
  bool negate_ = false;
  /** Only valid for OperandType::kImmediate32. */
  u32 immediate_[4];

  explicit Src(OperandType type, u32 swizzle) : OperandAddress(type), swizzle_(swizzle) {}
  explicit Src(OperandType type, u32 swizzle, Index index_1d)
      : OperandAddress(type, index_1d), swizzle_(swizzle) {}
  explicit Src(OperandType type, u32 swizzle, Index index_1d, Index index_2d)
      : OperandAddress(type, index_1d, index_2d), swizzle_(swizzle) {}
  explicit Src(OperandType type, u32 swizzle, Index index_1d, Index index_2d, Index index_3d)
      : OperandAddress(type, index_1d, index_2d, index_3d), swizzle_(swizzle) {}

  /** Tag for creating instances for use in declarations. */
  struct DclT {};
  static constexpr DclT Dcl = {};

  static Src R(u32 index, u32 swizzle = kXYZW) { return Src(OperandType::kTemp, swizzle, index); }
  static Src V1D(Index index, u32 swizzle = kXYZW) {
    return Src(OperandType::kInput, swizzle, index);
  }
  static Src V2D(Index index_1d, Index index_2d, u32 swizzle = kXYZW) {
    return Src(OperandType::kInput, swizzle, index_1d, index_2d);
  }
  static Src X(u32 index_1d, Index index_2d, u32 swizzle = kXYZW) {
    return Src(OperandType::kIndexableTemp, swizzle, index_1d, index_2d);
  }
  static Src LU(u32 x, u32 y, u32 z, u32 w) {
    Src src(OperandType::kImmediate32, kXYZW);
    src.immediate_[0] = x;
    src.immediate_[1] = y;
    src.immediate_[2] = z;
    src.immediate_[3] = w;
    return src;
  }
  static Src LU(u32 x) { return LU(x, x, x, x); }
  static Src LI(i32 x, i32 y, i32 z, i32 w) {
    return LU(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z), static_cast<u32>(w));
  }
  static Src LI(i32 x) { return LI(x, x, x, x); }
  static Src LF(f32 x, f32 y, f32 z, f32 w) {
    return LU(rex::memory::Reinterpret<u32>(x), rex::memory::Reinterpret<u32>(y),
              rex::memory::Reinterpret<u32>(z), rex::memory::Reinterpret<u32>(w));
  }
  static Src LF(f32 x) { return LF(x, x, x, x); }
  static Src LP(const u32* xyzw) { return LU(xyzw[0], xyzw[1], xyzw[2], xyzw[3]); }
  static Src LP(const i32* xyzw) { return LI(xyzw[0], xyzw[1], xyzw[2], xyzw[3]); }
  static Src LP(const f32* xyzw) { return LF(xyzw[0], xyzw[1], xyzw[2], xyzw[3]); }
  static Src S(u32 index_1d, Index index_2d) {
    return Src(OperandType::kSampler, kXXXX, index_1d, index_2d);
  }
  static Src S(DclT, u32 id, u32 lower_bound, u32 upper_bound) {
    return Src(OperandType::kSampler, kXYZW, id, lower_bound, upper_bound);
  }
  static Src T(u32 index_1d, Index index_2d, u32 swizzle = kXYZW) {
    return Src(OperandType::kResource, swizzle, index_1d, index_2d);
  }
  static Src T(DclT, u32 id, u32 lower_bound, u32 upper_bound) {
    return Src(OperandType::kResource, kXYZW, id, lower_bound, upper_bound);
  }
  static Src CB(u32 id, Index index, Index location, u32 swizzle = kXYZW) {
    return Src(OperandType::kConstantBuffer, swizzle, id, index, location);
  }
  static Src CB(DclT, u32 id, u32 lower_bound, u32 upper_bound) {
    return Src(OperandType::kConstantBuffer, kXYZW, id, lower_bound, upper_bound);
  }
  static Src Label(u32 index) { return Src(OperandType::kLabel, kXXXX, index); }
  static Src VPrim() { return Src(OperandType::kInputPrimitiveID, kXXXX); }
  static Src VICP(Index control_point, Index element, u32 swizzle = kXYZW) {
    return Src(OperandType::kInputControlPoint, swizzle, control_point, element);
  }
  static Src VDomain(u32 swizzle = kXYZW) { return Src(OperandType::kInputDomainPoint, swizzle); }
  static Src U(u32 index_1d, Index index_2d, u32 swizzle = kXYZW) {
    return Src(OperandType::kUnorderedAccessView, swizzle, index_1d, index_2d);
  }
  static Src U(DclT, u32 id, u32 lower_bound, u32 upper_bound) {
    return Src(OperandType::kUnorderedAccessView, kXYZW, id, lower_bound, upper_bound);
  }
  static Src VThreadID(u32 swizzle = kXYZW) { return Src(OperandType::kInputThreadID, swizzle); }
  static Src VThreadGroupID(u32 swizzle = kXYZW) {
    return Src(OperandType::kInputThreadGroupID, swizzle);
  }
  static Src VThreadIDInGroup(u32 swizzle = kXYZW) {
    return Src(OperandType::kInputThreadIDInGroup, swizzle);
  }
  static Src VCoverage() { return Src(OperandType::kInputCoverageMask, kXXXX); }

  [[nodiscard]] Src WithModifiers(bool absolute, bool negate) const {
    Src new_src(*this);
    new_src.absolute_ = absolute;
    new_src.negate_ = negate;
    return new_src;
  }
  [[nodiscard]] Src WithAbs(bool absolute) const { return WithModifiers(absolute, negate_); }
  [[nodiscard]] Src WithNeg(bool negate) const { return WithModifiers(absolute_, negate); }
  [[nodiscard]] Src Abs() const { return WithModifiers(true, false); }
  [[nodiscard]] Src operator-() const { return WithModifiers(absolute_, !negate_); }
  [[nodiscard]] Src Swizzle(u32 swizzle) const {
    Src new_src(*this);
    new_src.swizzle_ = swizzle;
    return new_src;
  }
  [[nodiscard]] Src SwizzleSwizzled(u32 swizzle) const {
    Src new_src(*this);
    new_src.swizzle_ = 0;
    for (u32 i = 0; i < 4; ++i) {
      new_src.swizzle_ |= ((swizzle_ >> (((swizzle >> (i * 2)) & 3) * 2)) & 3) << (i * 2);
    }
    return new_src;
  }
  [[nodiscard]] Src Select(u32 component) const {
    Src new_src(*this);
    new_src.swizzle_ = component * 0b01010101;
    return new_src;
  }
  [[nodiscard]] Src SelectFromSwizzled(u32 component) const {
    Src new_src(*this);
    new_src.swizzle_ = ((swizzle_ >> (component * 2)) & 3) * 0b01010101;
    return new_src;
  }

  u32 GetLength(u32 mask, bool force_vector = false) const {
    bool is_vector =
        force_vector || (mask != 0b0000 && Dest::GetMaskSingleComponent(mask) == UINT32_MAX);
    if (type_ == OperandType::kImmediate32) {
      return is_vector ? 5 : 2;
    }
    return ((absolute_ || negate_) ? 2 : 1) + OperandAddress::GetAddressLength();
  }
  static constexpr u32 GetModifiedImmediate(u32 value, bool is_integer, bool absolute,
                                            bool negate) {
    if (is_integer) {
      if (absolute) {
        value = static_cast<u32>(std::abs(static_cast<i32>(value)));
      }
      if (negate) {
        value = static_cast<u32>(-static_cast<i32>(value));
      }
    } else {
      if (absolute) {
        value &= static_cast<u32>(INT32_MAX);
      }
      if (negate) {
        value ^= static_cast<u32>(INT32_MAX) + 1;
      }
    }
    return value;
  }
  u32 GetModifiedImmediate(u32 swizzle_index, bool is_integer) const {
    return GetModifiedImmediate(immediate_[(swizzle_ >> (swizzle_index * 2)) & 3], is_integer,
                                absolute_, negate_);
  }
  /**
   * Appends the operand tokens.
   *
   * The swizzle of components outside the mask is set to a used component, so no potentially
   * uninitialized register component is referenced.
   *
   * @param code Code vector to append to.
   * @param is_integer True when immediates are integers, for applying modifiers.
   * @param mask Component mask of the destination.
   * @param force_vector True to write a vector operand even for a single component mask.
   * @param in_dcl True when written inside a declaration.
   */
  void Write(std::vector<u32>& code, bool is_integer, u32 mask, bool force_vector = false,
             bool in_dcl = false) const {
    u32 operand_token = GetOperandTokenTypeAndIndex();
    u32 mask_single_component = Dest::GetMaskSingleComponent(mask);
    u32 select_component = mask_single_component != UINT32_MAX ? mask_single_component : 0;
    bool is_vector = force_vector || (mask != 0b0000 && mask_single_component == UINT32_MAX);
    if (type_ == OperandType::kImmediate32) {
      if (is_vector) {
        operand_token |= std::to_underlying(OperandDimension::kVector) |
                         (std::to_underlying(ComponentSelection::kSwizzle) << 2) |
                         (Src::kXYZW << 4);
      } else {
        operand_token |= std::to_underlying(OperandDimension::kScalar);
      }
      code.push_back(operand_token);
      if (is_vector) {
        for (u32 i = 0; i < 4; ++i) {
          code.push_back((mask & (1 << i)) ? GetModifiedImmediate(i, is_integer) : 0);
        }
      } else {
        code.push_back(GetModifiedImmediate(select_component, is_integer));
      }
    } else {
      switch (GetDimension(in_dcl)) {
        case OperandDimension::kScalar:
          if (is_vector) {
            operand_token |= std::to_underlying(OperandDimension::kVector) |
                             (std::to_underlying(ComponentSelection::kSwizzle) << 2) |
                             (Src::kXXXX << 4);
          } else {
            operand_token |= std::to_underlying(OperandDimension::kScalar);
          }
          break;
        case OperandDimension::kVector:
          operand_token |= std::to_underlying(OperandDimension::kVector);
          if (is_vector) {
            operand_token |= std::to_underlying(ComponentSelection::kSwizzle) << 2;
            u32 used_component;
            if (!rex::bit_scan_forward(mask, &used_component)) {
              used_component = 0;
            }
            for (u32 i = 0; i < 4; ++i) {
              u32 swizzle_index = (mask & (1 << i)) ? i : used_component;
              operand_token |= (((swizzle_ >> (swizzle_index * 2)) & 3) << (4 + i * 2));
            }
          } else {
            operand_token |= (std::to_underlying(ComponentSelection::kSelect1) << 2) |
                             (((swizzle_ >> (select_component * 2)) & 3) << 4);
          }
          break;
        default:
          break;
      }
      OperandModifier modifier = OperandModifier::kNone;
      if (absolute_ && negate_) {
        modifier = OperandModifier::kAbsoluteNegate;
      } else if (absolute_) {
        modifier = OperandModifier::kAbsolute;
      } else if (negate_) {
        modifier = OperandModifier::kNegate;
      }
      if (modifier != OperandModifier::kNone) {
        operand_token |= static_cast<u32>(1) << 31;
      }
      code.push_back(operand_token);
      if (modifier != OperandModifier::kNone) {
        code.push_back(std::to_underlying(ExtendedOperandType::kModifier) |
                       (static_cast<u32>(modifier) << 6));
      }
      OperandAddress::Write(code);
    }
  }
};

// D3D10_SB_GLOBAL_FLAGS_MASK
enum GlobalFlags : u32 {
  /** Permits the driver to reorder arithmetic operations for optimization. */
  kGlobalFlagRefactoringAllowed = 1 << 11,
  kGlobalFlagEnableDoublePrecisionFloatOps = 1 << 12,
  kGlobalFlagForceEarlyDepthStencil = 1 << 13,
  /** Enables RAW and structured buffers in non CS 4.x shaders, not needed on 5.x. */
  kGlobalFlagEnableRawAndStructuredBuffers = 1 << 14,
  /** Direct3D 11.1. Skips optimizations of shader IL when translating to native code. */
  kGlobalFlagSkipOptimization = 1 << 15,
  kGlobalFlagEnableMinimumPrecision = 1 << 16,
  /** Enables 11.1 double precision floating point instruction extensions, not needed on 5.1. */
  kGlobalFlagEnableDoubleExtensions = 1 << 17,
  /** Enables 11.1 non double instruction extensions, not needed on 5.1. */
  kGlobalFlagEnableShaderExtensions = 1 << 18,
  /** Direct3D 12. */
  kGlobalFlagAllResourcesBound = 1 << 19,
};

// D3D10_SB_SAMPLER_MODE
enum class SamplerMode : u8 {
  kDefault,
  kComparison,
  kMono,
};

// D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN
enum class ConstantBufferAccessPattern : u8 {
  kImmediateIndexed,
  kDynamicIndexed,
};

// D3D10_SB_INTERPOLATION_MODE
enum class InterpolationMode : u8 {
  kUndefined,
  kConstant,
  kLinear,
  kLinearCentroid,
  kLinearNoPerspective,
  kLinearNoPerspectiveCentroid,
  kLinearSample,
  kLinearNoPerspectiveSample,
};

// D3D10_SB_RESOURCE_DIMENSION
enum class ResourceDimension : u8 {
  kUnknown,
  kBuffer,
  kTexture1D,
  kTexture2D,
  kTexture2DMS,
  kTexture3D,
  kTextureCube,
  kTexture1DArray,
  kTexture2DArray,
  kTexture2DMSArray,
  kTextureCubeArray,
  kRawBuffer,
  kStructuredBuffer,
};

// D3D11_SB_RESOURCE_FLAGS_MASK
enum UAVFlags : u32 {
  kUAVFlagGloballyCoherentAccess = 1 << 16,
  kUAVFlagRasterizerOrderedAccess = 1 << 17,
  kUAVFlagHasOrderPreservingCounter = 1 << 23,
};

// D3D10_SB_OPCODE_TYPE subset
enum class Opcode : u8 {
  kAdd = 0,
  kAnd = 1,
  kBreak = 2,
  kCall = 4,
  kCallC = 5,
  kCase = 6,
  kContinue = 7,
  kDefault = 10,
  kDiscard = 13,
  kDiv = 14,
  kDP2 = 15,
  kDP3 = 16,
  kDP4 = 17,
  kElse = 18,
  kEndIf = 21,
  kEndLoop = 22,
  kEndSwitch = 23,
  kEq = 24,
  kExp = 25,
  kFrc = 26,
  kFToI = 27,
  kFToU = 28,
  kGE = 29,
  kIAdd = 30,
  kIf = 31,
  kIEq = 32,
  kIGE = 33,
  kILT = 34,
  kIMAd = 35,
  kIMax = 36,
  kIMin = 37,
  kIMul = 38,
  kINE = 39,
  kIShL = 41,
  kIToF = 43,
  kLabel = 44,
  kLd = 45,
  kLdMS = 46,
  kLog = 47,
  kLoop = 48,
  kLT = 49,
  kMAd = 50,
  kMin = 51,
  kMax = 52,
  kCustomData = 53,
  kMov = 54,
  kMovC = 55,
  kMul = 56,
  kNE = 57,
  kNot = 59,
  kOr = 60,
  kRet = 62,
  kRetC = 63,
  kRoundNE = 64,
  kRoundNI = 65,
  kRoundZ = 67,
  kRSq = 68,
  kSampleL = 72,
  kSampleD = 73,
  kSqRt = 75,
  kSwitch = 76,
  kSinCos = 77,
  kUDiv = 78,
  kULT = 79,
  kUGE = 80,
  kUMul = 81,
  kUMAd = 82,
  kUMax = 83,
  kUMin = 84,
  kUShR = 85,
  kUToF = 86,
  kXOr = 87,
  kDclResource = 88,
  kDclConstantBuffer = 89,
  kDclSampler = 90,
  kDclOutputTopology = 92,
  kDclInputPrimitive = 93,
  kDclMaxOutputVertexCount = 94,
  kDclInput = 95,
  kDclInputSGV = 96,
  kDclInputSIV = 97,
  kDclInputPS = 98,
  kDclInputPSSGV = 99,
  kDclInputPSSIV = 100,
  kDclOutput = 101,
  kDclOutputSIV = 103,
  kDclTemps = 104,
  kDclIndexableTemp = 105,
  kDclGlobalFlags = 106,
  kLOD = 108,
  kEmitStream = 117,
  kCutStream = 118,
  kEmitThenCutStream = 119,
  kDerivRTXCoarse = 122,
  kDerivRTXFine = 123,
  kDerivRTYCoarse = 124,
  kDerivRTYFine = 125,
  kRcp = 129,
  kF32ToF16 = 130,
  kF16ToF32 = 131,
  kFirstBitHi = 135,
  kFirstBitLo = 136,
  kUBFE = 138,
  kIBFE = 139,
  kBFI = 140,
  kBFRev = 141,
  kDclStream = 143,
  kDclInputControlPointCount = 147,
  kDclTessDomain = 149,
  kDclThreadGroup = 155,
  kDclUnorderedAccessViewTyped = 156,
  kDclUnorderedAccessViewRaw = 157,
  kDclResourceRaw = 161,
  kLdUAVTyped = 163,
  kStoreUAVTyped = 164,
  kLdRaw = 165,
  kStoreRaw = 166,
  kAtomicAnd = 169,
  kAtomicOr = 170,
  kEvalSampleIndex = 204,
  kEvalCentroid = 205,
};

// D3D10_SB_EXTENDED_OPCODE_TYPE
enum class ExtendedOpcodeType : u8 {
  kEmpty,
  kSampleControls,
  kResourceDim,
  kResourceReturnType,
};

constexpr u32 OpcodeToken(Opcode opcode, u32 operands_length, bool saturate = false,
                          u32 extended_opcode_count = 0) {
  return static_cast<u32>(opcode) | (saturate ? (static_cast<u32>(1) << 13) : 0) |
         ((static_cast<u32>(1) + extended_opcode_count + operands_length) << 24) |
         (extended_opcode_count ? (static_cast<u32>(1) << 31) : 0);
}

constexpr u32 GetOpcodeTokenInstructionLength(u32 opcode_token) {
  return (opcode_token >> 24) & ((UINT32_C(1) << 7) - 1);
}

constexpr u32 SampleControlsExtendedOpcodeToken(i32 aoffimmi_u, i32 aoffimmi_v, i32 aoffimmi_w,
                                                bool extended = false) {
  return std::to_underlying(ExtendedOpcodeType::kSampleControls) |
         ((static_cast<u32>(aoffimmi_u) & static_cast<u32>(0b1111)) << 9) |
         ((static_cast<u32>(aoffimmi_v) & static_cast<u32>(0b1111)) << 13) |
         ((static_cast<u32>(aoffimmi_w) & static_cast<u32>(0b1111)) << 17) |
         (extended ? (static_cast<u32>(1) << 31) : 0);
}

constexpr u32 ResourceReturnTypeToken(ResourceReturnType x, ResourceReturnType y,
                                      ResourceReturnType z, ResourceReturnType w) {
  return static_cast<u32>(x) | (static_cast<u32>(y) << 4) | (static_cast<u32>(z) << 8) |
         (static_cast<u32>(w) << 12);
}

/**
 * Returns the resource return type token with the same type for all 4 components.
 *
 * A texture or typed buffer with fewer than 4 components has the same return type specified for
 * all 4 in its dcl instruction.
 *
 * @param xyzw Return type for all components.
 * @return Return type token.
 */
constexpr u32 ResourceReturnTypeX4Token(ResourceReturnType xyzw) {
  return ResourceReturnTypeToken(xyzw, xyzw, xyzw, xyzw);
}

/** Assembler appending to the shader program code vector. */
class Assembler {
 public:
  Assembler(std::vector<u32>& code, Statistics& stat) : code_(code), stat_(stat) {}

  void OpAdd(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    EmitAluOp(Opcode::kAdd, 0b00, dest, src0, src1, saturate);
    ++stat_.float_instruction_count;
  }
  void OpAnd(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kAnd, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpBreak() {
    code_.push_back(OpcodeToken(Opcode::kBreak, 0));
    ++stat_.instruction_count;
  }
  void OpCall(const Src& label) {
    EmitFlowOp(Opcode::kCall, label);
    ++stat_.static_flow_control_count;
  }
  void OpCallC(bool test, const Src& src, const Src& label) {
    EmitFlowOp(Opcode::kCallC, src, label, test);
    ++stat_.dynamic_flow_control_count;
  }
  void OpCase(const Src& src) {
    EmitFlowOp(Opcode::kCase, src);
    ++stat_.static_flow_control_count;
  }
  void OpContinue() {
    code_.push_back(OpcodeToken(Opcode::kContinue, 0));
    ++stat_.instruction_count;
  }
  void OpDefault() {
    code_.push_back(OpcodeToken(Opcode::kDefault, 0));
    ++stat_.instruction_count;
    ++stat_.static_flow_control_count;
  }
  void OpDiscard(bool test, const Src& src) { EmitFlowOp(Opcode::kDiscard, src, test); }
  void OpDiv(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    EmitAluOp(Opcode::kDiv, 0b00, dest, src0, src1, saturate);
    ++stat_.float_instruction_count;
  }
  void OpDP2(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    u32 operands_length = dest.GetLength() + src0.GetLength(0b0011) + src1.GetLength(0b0011);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDP2, operands_length, saturate));
    dest.Write(code_);
    src0.Write(code_, false, 0b0011);
    src1.Write(code_, false, 0b0011);
    ++stat_.instruction_count;
    ++stat_.float_instruction_count;
  }
  void OpDP3(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    u32 operands_length = dest.GetLength() + src0.GetLength(0b0111) + src1.GetLength(0b0111);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDP3, operands_length, saturate));
    dest.Write(code_);
    src0.Write(code_, false, 0b0111);
    src1.Write(code_, false, 0b0111);
    ++stat_.instruction_count;
    ++stat_.float_instruction_count;
  }
  void OpDP4(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    u32 operands_length = dest.GetLength() + src0.GetLength(0b1111) + src1.GetLength(0b1111);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDP4, operands_length, saturate));
    dest.Write(code_);
    src0.Write(code_, false, 0b1111);
    src1.Write(code_, false, 0b1111);
    ++stat_.instruction_count;
    ++stat_.float_instruction_count;
  }
  void OpElse() {
    code_.push_back(OpcodeToken(Opcode::kElse, 0));
    ++stat_.instruction_count;
  }
  void OpEndIf() {
    code_.push_back(OpcodeToken(Opcode::kEndIf, 0));
    ++stat_.instruction_count;
  }
  void OpEndLoop() {
    code_.push_back(OpcodeToken(Opcode::kEndLoop, 0));
    ++stat_.instruction_count;
  }
  void OpEndSwitch() {
    code_.push_back(OpcodeToken(Opcode::kEndSwitch, 0));
    ++stat_.instruction_count;
  }
  void OpEq(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kEq, 0b00, dest, src0, src1);
    ++stat_.float_instruction_count;
  }
  void OpExp(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kExp, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpFrc(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kFrc, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpFToI(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kFToI, 0b0, dest, src);
    ++stat_.conversion_instruction_count;
  }
  void OpFToU(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kFToU, 0b0, dest, src);
    ++stat_.conversion_instruction_count;
  }
  void OpGE(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kGE, 0b00, dest, src0, src1);
    ++stat_.float_instruction_count;
  }
  void OpIAdd(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kIAdd, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpIf(bool test, const Src& src) {
    EmitFlowOp(Opcode::kIf, src, test);
    ++stat_.dynamic_flow_control_count;
  }
  void OpIEq(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kIEq, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpIGE(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kIGE, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpILT(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kILT, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpIMAd(const Dest& dest, const Src& mul0, const Src& mul1, const Src& add) {
    EmitAluOp(Opcode::kIMAd, 0b111, dest, mul0, mul1, add);
    ++stat_.int_instruction_count;
  }
  void OpIMax(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kIMax, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpIMin(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kIMin, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpIMul(const Dest& dest_hi, const Dest& dest_lo, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kIMul, 0b11, dest_hi, dest_lo, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpINE(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kINE, 0b11, dest, src0, src1);
    ++stat_.int_instruction_count;
  }
  void OpIShL(const Dest& dest, const Src& value, const Src& shift) {
    EmitAluOp(Opcode::kIShL, 0b11, dest, value, shift);
    ++stat_.int_instruction_count;
  }
  void OpIToF(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kIToF, 0b1, dest, src);
    ++stat_.conversion_instruction_count;
  }
  /**
   * Emits a label.
   *
   * The label is a source operand as in call and callc, while in DXBC it is only a 0 component
   * label operand. It does not count towards stat_.instruction_count.
   *
   * @param label Label operand.
   */
  void OpLabel(const Src& label) {
    u32 operands_length = label.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kLabel, operands_length));
    label.Write(code_, true, 0b0000);
  }
  void OpLd(const Dest& dest, const Src& address, u32 address_mask, const Src& resource,
            i32 aoffimmi_u = 0, i32 aoffimmi_v = 0, i32 aoffimmi_w = 0) {
    u32 dest_write_mask = dest.GetMask();
    u32 sample_controls = 0;
    if (aoffimmi_u || aoffimmi_v || aoffimmi_w) {
      sample_controls = SampleControlsExtendedOpcodeToken(aoffimmi_u, aoffimmi_v, aoffimmi_w);
    }
    u32 operands_length = dest.GetLength() + address.GetLength(address_mask, true) +
                          resource.GetLength(dest_write_mask, true);
    code_.reserve(code_.size() + 1 + (sample_controls ? 1 : 0) + operands_length);
    code_.push_back(OpcodeToken(Opcode::kLd, operands_length, false, sample_controls ? 1 : 0));
    if (sample_controls) {
      code_.push_back(sample_controls);
    }
    dest.Write(code_);
    address.Write(code_, false, address_mask, true);
    resource.Write(code_, false, dest_write_mask, true);
    ++stat_.instruction_count;
    ++stat_.texture_load_instructions;
  }
  void OpLdMS(const Dest& dest, const Src& address, u32 address_mask, const Src& resource,
              const Src& sample_index, i32 aoffimmi_u = 0, i32 aoffimmi_v = 0) {
    u32 dest_write_mask = dest.GetMask();
    u32 sample_controls = 0;
    if (aoffimmi_u || aoffimmi_v) {
      sample_controls = SampleControlsExtendedOpcodeToken(aoffimmi_u, aoffimmi_v, 0);
    }
    u32 operands_length = dest.GetLength() + address.GetLength(address_mask, true) +
                          resource.GetLength(dest_write_mask, true) +
                          sample_index.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + (sample_controls ? 1 : 0) + operands_length);
    code_.push_back(OpcodeToken(Opcode::kLdMS, operands_length, false, sample_controls ? 1 : 0));
    if (sample_controls) {
      code_.push_back(sample_controls);
    }
    dest.Write(code_);
    address.Write(code_, false, address_mask, true);
    resource.Write(code_, false, dest_write_mask, true);
    sample_index.Write(code_, true, 0b0000);
    ++stat_.instruction_count;
    ++stat_.texture_load_instructions;
  }
  void OpLog(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kLog, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpLoop() {
    code_.push_back(OpcodeToken(Opcode::kLoop, 0));
    ++stat_.instruction_count;
    ++stat_.dynamic_flow_control_count;
  }
  void OpLT(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kLT, 0b00, dest, src0, src1);
    ++stat_.float_instruction_count;
  }
  void OpMAd(const Dest& dest, const Src& mul0, const Src& mul1, const Src& add,
             bool saturate = false) {
    EmitAluOp(Opcode::kMAd, 0b000, dest, mul0, mul1, add, saturate);
    ++stat_.float_instruction_count;
  }
  void OpMin(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    EmitAluOp(Opcode::kMin, 0b00, dest, src0, src1, saturate);
    ++stat_.float_instruction_count;
  }
  void OpMax(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    EmitAluOp(Opcode::kMax, 0b00, dest, src0, src1, saturate);
    ++stat_.float_instruction_count;
  }
  /**
   * Reserves custom data in the code and returns a pointer for writing it.
   *
   * The opcode token has no size field. The padding is filled with kAlignmentPadding so equal
   * input gives equal DXBC for driver shader caching.
   *
   * @param custom_data_class Custom data class.
   * @param length_bytes Length of the data in bytes.
   * @return Pointer to the data to write.
   */
  void* OpCustomData(CustomDataClass custom_data_class, u32 length_bytes) {
    u32 length_bytes_aligned = rex::align(length_bytes, static_cast<u32>(sizeof(u32)));
    u32 total_length_dwords = length_bytes_aligned / sizeof(u32) + 2;
    size_t offset_dwords = code_.size();
    code_.resize(offset_dwords + total_length_dwords);
    u32* data = code_.data() + offset_dwords;
    *(data++) = static_cast<u32>(Opcode::kCustomData) | (static_cast<u32>(custom_data_class) << 11);
    *(data++) = total_length_dwords;
    std::memset(reinterpret_cast<u8*>(data) + length_bytes, dxbc::kAlignmentPadding,
                length_bytes_aligned - length_bytes);
    return data;
  }
  void OpMov(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kMov, 0b0, dest, src, saturate);
    if (dest.type_ == OperandType::kIndexableTemp || src.type_ == OperandType::kIndexableTemp) {
      ++stat_.array_instruction_count;
    } else {
      ++stat_.mov_instruction_count;
    }
  }
  void OpMovC(const Dest& dest, const Src& test, const Src& src_nz, const Src& src_z,
              bool saturate = false) {
    EmitAluOp(Opcode::kMovC, 0b001, dest, test, src_nz, src_z, saturate);
    ++stat_.movc_instruction_count;
  }
  void OpMul(const Dest& dest, const Src& src0, const Src& src1, bool saturate = false) {
    EmitAluOp(Opcode::kMul, 0b00, dest, src0, src1, saturate);
    ++stat_.float_instruction_count;
  }
  void OpNE(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kNE, 0b00, dest, src0, src1);
    ++stat_.float_instruction_count;
  }
  void OpNot(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kNot, 0b1, dest, src);
    ++stat_.uint_instruction_count;
  }
  void OpOr(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kOr, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpRet() {
    code_.push_back(OpcodeToken(Opcode::kRet, 0));
    ++stat_.instruction_count;
    ++stat_.static_flow_control_count;
  }
  void OpRetC(bool test, const Src& src) {
    EmitFlowOp(Opcode::kRetC, src, test);
    ++stat_.dynamic_flow_control_count;
  }
  void OpRoundNE(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kRoundNE, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpRoundNI(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kRoundNI, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpRoundZ(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kRoundZ, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpRSq(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kRSq, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpSampleL(const Dest& dest, const Src& address, u32 address_components, const Src& resource,
                 const Src& sampler, const Src& lod, i32 aoffimmi_u = 0, i32 aoffimmi_v = 0,
                 i32 aoffimmi_w = 0) {
    u32 dest_write_mask = dest.GetMask();
    u32 sample_controls = 0;
    if (aoffimmi_u || aoffimmi_v || aoffimmi_w) {
      sample_controls = SampleControlsExtendedOpcodeToken(aoffimmi_u, aoffimmi_v, aoffimmi_w);
    }
    u32 address_mask = (1 << address_components) - 1;
    u32 operands_length = dest.GetLength() + address.GetLength(address_mask) +
                          resource.GetLength(dest_write_mask, true) + sampler.GetLength(0b0000) +
                          lod.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + (sample_controls ? 1 : 0) + operands_length);
    code_.push_back(OpcodeToken(Opcode::kSampleL, operands_length, false, sample_controls ? 1 : 0));
    if (sample_controls) {
      code_.push_back(sample_controls);
    }
    dest.Write(code_);
    address.Write(code_, false, address_mask);
    resource.Write(code_, false, dest_write_mask, true);
    sampler.Write(code_, false, 0b0000);
    lod.Write(code_, false, 0b0000);
    ++stat_.instruction_count;
    ++stat_.texture_normal_instructions;
  }
  /**
   * Emits a sample with explicit derivatives.
   *
   * A 1 component address has 1 component derivatives and a 4 component address has 4 component
   * derivatives.
   *
   * @param dest Destination.
   * @param address Address source.
   * @param address_components Address component count.
   * @param resource Resource source.
   * @param sampler Sampler source.
   * @param x_derivatives Derivatives along X.
   * @param y_derivatives Derivatives along Y.
   * @param derivatives_components Derivative component count.
   * @param aoffimmi_u Immediate offset along U.
   * @param aoffimmi_v Immediate offset along V.
   * @param aoffimmi_w Immediate offset along W.
   */
  void OpSampleD(const Dest& dest, const Src& address, u32 address_components, const Src& resource,
                 const Src& sampler, const Src& x_derivatives, const Src& y_derivatives,
                 u32 derivatives_components, i32 aoffimmi_u = 0, i32 aoffimmi_v = 0,
                 i32 aoffimmi_w = 0) {
    REX_ASSERT(derivatives_components <= address_components);
    u32 dest_write_mask = dest.GetMask();
    u32 sample_controls = 0;
    if (aoffimmi_u || aoffimmi_v || aoffimmi_w) {
      sample_controls = SampleControlsExtendedOpcodeToken(aoffimmi_u, aoffimmi_v, aoffimmi_w);
    }
    u32 address_mask = (1 << address_components) - 1;
    u32 derivatives_mask = (1 << derivatives_components) - 1;
    u32 operands_length = dest.GetLength() + address.GetLength(address_mask) +
                          resource.GetLength(dest_write_mask, true) + sampler.GetLength(0b0000) +
                          x_derivatives.GetLength(derivatives_mask, address_components > 1) +
                          y_derivatives.GetLength(derivatives_mask, address_components > 1);
    code_.reserve(code_.size() + 1 + (sample_controls ? 1 : 0) + operands_length);
    code_.push_back(OpcodeToken(Opcode::kSampleD, operands_length, false, sample_controls ? 1 : 0));
    if (sample_controls) {
      code_.push_back(sample_controls);
    }
    dest.Write(code_);
    address.Write(code_, false, address_mask);
    resource.Write(code_, false, dest_write_mask, true);
    sampler.Write(code_, false, 0b0000);
    x_derivatives.Write(code_, false, derivatives_mask, address_components > 1);
    y_derivatives.Write(code_, false, derivatives_mask, address_components > 1);
    ++stat_.instruction_count;
    ++stat_.texture_gradient_instructions;
  }
  void OpSqRt(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kSqRt, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpSwitch(const Src& src) {
    EmitFlowOp(Opcode::kSwitch, src);
    ++stat_.dynamic_flow_control_count;
  }
  void OpSinCos(const Dest& dest_sin, const Dest& dest_cos, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kSinCos, 0b0, dest_sin, dest_cos, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpUDiv(const Dest& dest_quotient, const Dest& dest_remainder, const Src& src0,
              const Src& src1) {
    EmitAluOp(Opcode::kUDiv, 0b11, dest_quotient, dest_remainder, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpULT(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kULT, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpUGE(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kUGE, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpUMul(const Dest& dest_hi, const Dest& dest_lo, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kUMul, 0b11, dest_hi, dest_lo, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpUMAd(const Dest& dest, const Src& mul0, const Src& mul1, const Src& add) {
    EmitAluOp(Opcode::kUMAd, 0b111, dest, mul0, mul1, add);
    ++stat_.uint_instruction_count;
  }
  void OpUMax(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kUMax, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpUMin(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kUMin, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpUShR(const Dest& dest, const Src& value, const Src& shift) {
    EmitAluOp(Opcode::kUShR, 0b11, dest, value, shift);
    ++stat_.uint_instruction_count;
  }
  void OpUToF(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kUToF, 0b1, dest, src);
    ++stat_.conversion_instruction_count;
  }
  void OpXOr(const Dest& dest, const Src& src0, const Src& src1) {
    EmitAluOp(Opcode::kXOr, 0b11, dest, src0, src1);
    ++stat_.uint_instruction_count;
  }
  void OpDclResource(ResourceDimension dimension, u32 return_type_token, const Src& operand,
                     u32 space = 0) {
    u32 operands_length = operand.GetLength(0b1111, false);
    code_.reserve(code_.size() + 3 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclResource, 2 + operands_length) |
                    (static_cast<u32>(dimension) << 11));
    operand.Write(code_, false, 0b1111, false, true);
    code_.push_back(return_type_token);
    code_.push_back(space);
  }
  /**
   * Declares a constant buffer.
   *
   * The order of constant buffer declarations in a shader is a hint to the driver of their
   * relative priority from highest to lowest.
   *
   * @param operand Constant buffer operand.
   * @param size_vectors Size in vectors.
   * @param access_pattern Access pattern.
   * @param space Register space.
   */
  void OpDclConstantBuffer(
      const Src& operand, u32 size_vectors,
      ConstantBufferAccessPattern access_pattern = ConstantBufferAccessPattern::kImmediateIndexed,
      u32 space = 0) {
    u32 operands_length = operand.GetLength(0b1111, false);
    code_.reserve(code_.size() + 3 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclConstantBuffer, 2 + operands_length) |
                    (static_cast<u32>(access_pattern) << 11));
    operand.Write(code_, false, 0b1111, false, true);
    code_.push_back(size_vectors);
    code_.push_back(space);
  }
  void OpDclSampler(const Src& operand, SamplerMode mode = SamplerMode::kDefault, u32 space = 0) {
    u32 operands_length = operand.GetLength(0b1111, false);
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclSampler, 1 + operands_length) |
                    (static_cast<u32>(mode) << 11));
    operand.Write(code_, false, 0b1111, false, true);
    code_.push_back(space);
  }
  /**
   * Declares the geometry shader output topology.
   *
   * Only kPointList, kLineStrip and kTriangleStrip are allowed in geometry shaders.
   *
   * @param output_topology Output topology.
   */
  void OpDclOutputTopology(PrimitiveTopology output_topology) {
    code_.push_back(OpcodeToken(Opcode::kDclOutputTopology, 0) |
                    (static_cast<u32>(output_topology) << 11));
    stat_.gs_output_topology = output_topology;
  }
  /**
   * Declares the geometry shader input primitive.
   *
   * Only kPoint, kLine, kTriangle, kLineWithAdjacency and kTriangleWithAdjacency are allowed in
   * geometry shaders.
   *
   * @param input_primitive Input primitive.
   */
  void OpDclInputPrimitive(Primitive input_primitive) {
    code_.push_back(OpcodeToken(Opcode::kDclInputPrimitive, 0) |
                    (static_cast<u32>(input_primitive) << 11));
    stat_.input_primitive = input_primitive;
  }
  /**
   * Declares the maximum output vertex count.
   *
   * @param count Maximum vertex count.
   * @return Index of the count written in the code_ vector.
   */
  size_t OpDclMaxOutputVertexCount(u32 count) {
    code_.reserve(code_.size() + 2);
    code_.push_back(OpcodeToken(Opcode::kDclMaxOutputVertexCount, 1));
    code_.push_back(count);
    stat_.gs_max_output_vertex_count = count;
    return code_.size() - 1;
  }
  void OpDclInput(const Dest& operand) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclInput, operands_length));
    operand.Write(code_, true);
    ++stat_.dcl_count;
  }
  void OpDclInputSGV(const Dest& operand, Name name) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclInputSGV, 1 + operands_length));
    operand.Write(code_, true);
    code_.push_back(static_cast<u32>(name));
    ++stat_.dcl_count;
  }
  void OpDclInputSIV(const Dest& operand, Name name) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclInputSIV, 1 + operands_length));
    operand.Write(code_, true);
    code_.push_back(static_cast<u32>(name));
    ++stat_.dcl_count;
  }
  void OpDclInputPS(InterpolationMode interpolation_mode, const Dest& operand) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclInputPS, operands_length) |
                    (static_cast<u32>(interpolation_mode) << 11));
    operand.Write(code_, true);
    ++stat_.dcl_count;
  }
  /**
   * Declares a pixel shader system generated value input.
   *
   * Constant interpolation mode is encoded, as FXC does at least for SV_IsFrontFace, although
   * d3d12TokenizedProgramFormat.hpp says bits 11:23 are ignored.
   *
   * @param operand Input operand.
   * @param name System value name.
   */
  void OpDclInputPSSGV(const Dest& operand, Name name) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclInputPSSGV, 1 + operands_length) |
                    (std::to_underlying(InterpolationMode::kConstant) << 11));
    operand.Write(code_, true);
    code_.push_back(static_cast<u32>(name));
    ++stat_.dcl_count;
  }
  void OpDclInputPSSIV(InterpolationMode interpolation_mode, const Dest& operand, Name name) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclInputPSSIV, 1 + operands_length) |
                    (static_cast<u32>(interpolation_mode) << 11));
    operand.Write(code_, true);
    code_.push_back(static_cast<u32>(name));
    ++stat_.dcl_count;
  }
  void OpDclOutput(const Dest& operand) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclOutput, operands_length));
    operand.Write(code_, true);
    ++stat_.dcl_count;
  }
  void OpDclOutputSIV(const Dest& operand, Name name) {
    u32 operands_length = operand.GetLength();
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclOutputSIV, 1 + operands_length));
    operand.Write(code_, true);
    code_.push_back(static_cast<u32>(name));
    ++stat_.dcl_count;
  }
  /**
   * Declares the temporary register count.
   *
   * @param count Temporary register count.
   * @return Index of the count written in the code_ vector.
   */
  size_t OpDclTemps(u32 count) {
    code_.reserve(code_.size() + 2);
    code_.push_back(OpcodeToken(Opcode::kDclTemps, 1));
    code_.push_back(count);
    stat_.temp_register_count = count;
    return code_.size() - 1;
  }
  void OpDclIndexableTemp(u32 index, u32 count, u32 component_count) {
    code_.reserve(code_.size() + 4);
    code_.push_back(OpcodeToken(Opcode::kDclIndexableTemp, 3));
    code_.push_back(index);
    code_.push_back(count);
    code_.push_back(component_count);
    stat_.temp_array_count += count;
  }
  /**
   * Declares the global flags.
   *
   * @param flags GlobalFlags bits.
   */
  void OpDclGlobalFlags(u32 flags) {
    code_.push_back(OpcodeToken(Opcode::kDclGlobalFlags, 0) | flags);
  }
  void OpLOD(const Dest& dest, const Src& address, u32 address_components, const Src& resource,
             const Src& sampler) {
    u32 dest_write_mask = dest.GetMask();
    u32 address_mask = (1 << address_components) - 1;
    u32 operands_length = dest.GetLength() + address.GetLength(address_mask) +
                          resource.GetLength(dest_write_mask) + sampler.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kLOD, operands_length));
    dest.Write(code_);
    address.Write(code_, false, address_mask);
    resource.Write(code_, false, dest_write_mask);
    sampler.Write(code_, false, 0b0000);
    ++stat_.instruction_count;
    ++stat_.lod_instructions;
  }
  void OpEmitStream(const Dest& stream) {
    u32 operands_length = stream.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kEmitStream, operands_length));
    stream.Write(code_);
    ++stat_.instruction_count;
    ++stat_.emit_instruction_count;
  }
  void OpCutStream(const Dest& stream) {
    u32 operands_length = stream.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kCutStream, operands_length));
    stream.Write(code_);
    ++stat_.instruction_count;
    ++stat_.cut_instruction_count;
  }
  /**
   * Emits a vertex and then cuts the stream.
   *
   * Crashes the AMD shader compiler when used conditionally, so OpEmitStream and OpCutStream are
   * used instead.
   *
   * @param stream Stream operand.
   */
  void OpEmitThenCutStream(const Dest& stream) {
    u32 operands_length = stream.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kEmitThenCutStream, operands_length));
    stream.Write(code_);
    ++stat_.instruction_count;
    ++stat_.emit_instruction_count;
    ++stat_.cut_instruction_count;
  }
  void OpDerivRTXCoarse(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kDerivRTXCoarse, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpDerivRTXFine(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kDerivRTXFine, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpDerivRTYCoarse(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kDerivRTYCoarse, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpDerivRTYFine(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kDerivRTYFine, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpRcp(const Dest& dest, const Src& src, bool saturate = false) {
    EmitAluOp(Opcode::kRcp, 0b0, dest, src, saturate);
    ++stat_.float_instruction_count;
  }
  void OpF32ToF16(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kF32ToF16, 0b0, dest, src);
    ++stat_.conversion_instruction_count;
  }
  void OpF16ToF32(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kF16ToF32, 0b1, dest, src);
    ++stat_.conversion_instruction_count;
  }
  void OpFirstBitHi(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kFirstBitHi, 0b1, dest, src);
    ++stat_.uint_instruction_count;
  }
  void OpFirstBitLo(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kFirstBitLo, 0b1, dest, src);
    ++stat_.uint_instruction_count;
  }
  void OpUBFE(const Dest& dest, const Src& width, const Src& offset, const Src& src) {
    EmitAluOp(Opcode::kUBFE, 0b111, dest, width, offset, src);
    ++stat_.uint_instruction_count;
  }
  void OpIBFE(const Dest& dest, const Src& width, const Src& offset, const Src& src) {
    EmitAluOp(Opcode::kIBFE, 0b111, dest, width, offset, src);
    ++stat_.int_instruction_count;
  }
  void OpBFI(const Dest& dest, const Src& width, const Src& offset, const Src& from,
             const Src& to) {
    EmitAluOp(Opcode::kBFI, 0b1111, dest, width, offset, from, to);
    ++stat_.uint_instruction_count;
  }
  void OpBFRev(const Dest& dest, const Src& src) {
    EmitAluOp(Opcode::kBFRev, 0b1, dest, src);
    ++stat_.uint_instruction_count;
  }
  void OpDclStream(const Dest& stream) {
    u32 operands_length = stream.GetLength();
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclStream, operands_length));
    stream.Write(code_, true);
  }
  void OpDclInputControlPointCount(u32 count) {
    code_.push_back(OpcodeToken(Opcode::kDclInputControlPointCount, 0) | (count << 11));
    stat_.c_control_points = count;
  }
  void OpDclTessDomain(TessellatorDomain domain) {
    code_.push_back(OpcodeToken(Opcode::kDclTessDomain, 0) | (static_cast<u32>(domain) << 11));
    stat_.tessellator_domain = domain;
  }
  void OpDclThreadGroup(u32 x, u32 y, u32 z) {
    code_.reserve(code_.size() + 4);
    code_.push_back(OpcodeToken(Opcode::kDclThreadGroup, 3));
    code_.push_back(x);
    code_.push_back(y);
    code_.push_back(z);
  }
  /**
   * Declares a typed unordered access view.
   *
   * @param dimension Resource dimension.
   * @param flags kUAVFlagGloballyCoherentAccess and kUAVFlagRasterizerOrderedAccess bits.
   * @param return_type_token Return type token.
   * @param operand UAV operand.
   * @param space Register space.
   */
  void OpDclUnorderedAccessViewTyped(ResourceDimension dimension, u32 flags, u32 return_type_token,
                                     const Src& operand, u32 space = 0) {
    u32 operands_length = operand.GetLength(0b1111, false);
    code_.reserve(code_.size() + 3 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclUnorderedAccessViewTyped, 2 + operands_length) |
                    (static_cast<u32>(dimension) << 11) | flags);
    operand.Write(code_, false, 0b1111, false, true);
    code_.push_back(return_type_token);
    code_.push_back(space);
  }
  /**
   * Declares a raw unordered access view.
   *
   * @param flags kUAVFlagGloballyCoherentAccess and kUAVFlagRasterizerOrderedAccess bits.
   * @param operand UAV operand.
   * @param space Register space.
   */
  void OpDclUnorderedAccessViewRaw(u32 flags, const Src& operand, u32 space = 0) {
    u32 operands_length = operand.GetLength(0b1111, false);
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclUnorderedAccessViewRaw, 1 + operands_length) | flags);
    operand.Write(code_, true, 0b1111, false, true);
    code_.push_back(space);
  }
  void OpDclResourceRaw(const Src& operand, u32 space = 0) {
    u32 operands_length = operand.GetLength(0b1111, false);
    code_.reserve(code_.size() + 2 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kDclResourceRaw, 1 + operands_length));
    operand.Write(code_, true, 0b1111, false, true);
    code_.push_back(space);
  }
  void OpLdUAVTyped(const Dest& dest, const Src& address, u32 address_components, const Src& uav) {
    u32 dest_write_mask = dest.GetMask();
    u32 address_mask = (1 << address_components) - 1;
    u32 operands_length = dest.GetLength() + address.GetLength(address_mask, true) +
                          uav.GetLength(dest_write_mask, true);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kLdUAVTyped, operands_length));
    dest.Write(code_);
    address.Write(code_, true, address_mask, true);
    uav.Write(code_, false, dest_write_mask, true);
    ++stat_.instruction_count;
    ++stat_.texture_load_instructions;
  }
  void OpStoreUAVTyped(const Dest& dest, const Src& address, u32 address_components,
                       const Src& value) {
    u32 dest_write_mask = dest.GetMask();
    // Typed UAV writes do not support write masking.
    REX_ASSERT(dest_write_mask == 0b1111);
    u32 address_mask = (1 << address_components) - 1;
    u32 operands_length =
        dest.GetLength() + address.GetLength(address_mask, true) + value.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kStoreUAVTyped, operands_length));
    dest.Write(code_);
    address.Write(code_, true, address_mask, true);
    value.Write(code_, false, dest_write_mask);
    ++stat_.instruction_count;
    ++stat_.c_texture_store_instructions;
  }
  /**
   * Emits a raw load.
   *
   * For Load, FXC writes to any component of the destination with the xxxx swizzle of the source
   * SRV or UAV. For Load2, Load3 and Load4 it uses the xy, xyz and xyzw write masks with the
   * xyxx, xyzx and xyzw swizzles.
   *
   * @param dest Destination.
   * @param byte_offset Byte offset source.
   * @param src SRV or UAV source.
   */
  void OpLdRaw(const Dest& dest, const Src& byte_offset, const Src& src) {
    u32 dest_write_mask = dest.GetMask();
    REX_ASSERT(dest_write_mask == 0b0001 || dest_write_mask == 0b0010 ||
               dest_write_mask == 0b0100 || dest_write_mask == 0b1000 ||
               dest_write_mask == 0b0011 || dest_write_mask == 0b0111 || dest_write_mask == 0b1111);
    u32 component_count = rex::bit_count(dest_write_mask);
    REX_ASSERT((src.swizzle_ & ((1 << (component_count * 2)) - 1)) ==
               (Src::kXYZW & ((1 << (component_count * 2)) - 1)));
    u32 src_mask = (1 << component_count) - 1;
    u32 operands_length =
        dest.GetLength() + byte_offset.GetLength(0b0000) + src.GetLength(src_mask, true);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kLdRaw, operands_length));
    dest.Write(code_);
    byte_offset.Write(code_, true, 0b0000);
    src.Write(code_, true, src_mask, true);
    ++stat_.instruction_count;
    ++stat_.texture_load_instructions;
  }
  void OpStoreRaw(const Dest& dest, const Src& byte_offset, const Src& value) {
    u32 dest_write_mask = dest.GetMask();
    REX_ASSERT(dest_write_mask == 0b0001 || dest_write_mask == 0b0011 ||
               dest_write_mask == 0b0111 || dest_write_mask == 0b1111);
    u32 operands_length =
        dest.GetLength() + byte_offset.GetLength(0b0000) + value.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kStoreRaw, operands_length));
    dest.Write(code_);
    byte_offset.Write(code_, true, 0b0000);
    value.Write(code_, true, dest_write_mask);
    ++stat_.instruction_count;
    ++stat_.c_texture_store_instructions;
  }
  void OpAtomicAnd(const Dest& dest, const Src& address, u32 address_components, const Src& value) {
    EmitAtomicOp(Opcode::kAtomicAnd, dest, address, address_components, value);
  }
  void OpAtomicOr(const Dest& dest, const Src& address, u32 address_components, const Src& value) {
    EmitAtomicOp(Opcode::kAtomicOr, dest, address, address_components, value);
  }
  void OpEvalSampleIndex(const Dest& dest, const Src& value, const Src& sample_index) {
    u32 dest_write_mask = dest.GetMask();
    u32 operands_length =
        dest.GetLength() + value.GetLength(dest_write_mask) + sample_index.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kEvalSampleIndex, operands_length));
    dest.Write(code_);
    value.Write(code_, false, dest_write_mask);
    sample_index.Write(code_, true, 0b0000);
    ++stat_.instruction_count;
  }
  void OpEvalCentroid(const Dest& dest, const Src& value) {
    u32 dest_write_mask = dest.GetMask();
    u32 operands_length = dest.GetLength() + value.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(Opcode::kEvalCentroid, operands_length));
    dest.Write(code_);
    value.Write(code_, false, dest_write_mask);
    ++stat_.instruction_count;
  }

 private:
  void EmitAluOp(Opcode opcode, u32 src_are_integer, const Dest& dest, const Src& src,
                 bool saturate = false) {
    u32 dest_write_mask = dest.GetMask();
    u32 operands_length = dest.GetLength() + src.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length, saturate));
    dest.Write(code_);
    src.Write(code_, (src_are_integer & 0b1) != 0, dest_write_mask);
    ++stat_.instruction_count;
  }
  void EmitAluOp(Opcode opcode, u32 src_are_integer, const Dest& dest, const Src& src0,
                 const Src& src1, bool saturate = false) {
    u32 dest_write_mask = dest.GetMask();
    u32 operands_length =
        dest.GetLength() + src0.GetLength(dest_write_mask) + src1.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length, saturate));
    dest.Write(code_);
    src0.Write(code_, (src_are_integer & 0b1) != 0, dest_write_mask);
    src1.Write(code_, (src_are_integer & 0b10) != 0, dest_write_mask);
    ++stat_.instruction_count;
  }
  void EmitAluOp(Opcode opcode, u32 src_are_integer, const Dest& dest, const Src& src0,
                 const Src& src1, const Src& src2, bool saturate = false) {
    u32 dest_write_mask = dest.GetMask();
    u32 operands_length = dest.GetLength() + src0.GetLength(dest_write_mask) +
                          src1.GetLength(dest_write_mask) + src2.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length, saturate));
    dest.Write(code_);
    src0.Write(code_, (src_are_integer & 0b1) != 0, dest_write_mask);
    src1.Write(code_, (src_are_integer & 0b10) != 0, dest_write_mask);
    src2.Write(code_, (src_are_integer & 0b100) != 0, dest_write_mask);
    ++stat_.instruction_count;
  }
  void EmitAluOp(Opcode opcode, u32 src_are_integer, const Dest& dest, const Src& src0,
                 const Src& src1, const Src& src2, const Src& src3, bool saturate = false) {
    u32 dest_write_mask = dest.GetMask();
    u32 operands_length = dest.GetLength() + src0.GetLength(dest_write_mask) +
                          src1.GetLength(dest_write_mask) + src2.GetLength(dest_write_mask) +
                          src3.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length, saturate));
    dest.Write(code_);
    src0.Write(code_, (src_are_integer & 0b1) != 0, dest_write_mask);
    src1.Write(code_, (src_are_integer & 0b10) != 0, dest_write_mask);
    src2.Write(code_, (src_are_integer & 0b100) != 0, dest_write_mask);
    src3.Write(code_, (src_are_integer & 0b1000) != 0, dest_write_mask);
    ++stat_.instruction_count;
  }
  void EmitAluOp(Opcode opcode, u32 src_are_integer, const Dest& dest0, const Dest& dest1,
                 const Src& src, bool saturate = false) {
    u32 dest_write_mask = dest0.GetMask() | dest1.GetMask();
    u32 operands_length = dest0.GetLength() + dest1.GetLength() + src.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length, saturate));
    dest0.Write(code_);
    dest1.Write(code_);
    src.Write(code_, (src_are_integer & 0b1) != 0, dest_write_mask);
    ++stat_.instruction_count;
  }
  void EmitAluOp(Opcode opcode, u32 src_are_integer, const Dest& dest0, const Dest& dest1,
                 const Src& src0, const Src& src1, bool saturate = false) {
    u32 dest_write_mask = dest0.GetMask() | dest1.GetMask();
    u32 operands_length = dest0.GetLength() + dest1.GetLength() + src0.GetLength(dest_write_mask) +
                          src1.GetLength(dest_write_mask);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length, saturate));
    dest0.Write(code_);
    dest1.Write(code_);
    src0.Write(code_, (src_are_integer & 0b1) != 0, dest_write_mask);
    src1.Write(code_, (src_are_integer & 0b10) != 0, dest_write_mask);
    ++stat_.instruction_count;
  }
  void EmitFlowOp(Opcode opcode, const Src& src, bool test = false) {
    u32 operands_length = src.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length) | (test ? (1 << 18) : 0));
    src.Write(code_, true, 0b0000);
    ++stat_.instruction_count;
  }
  void EmitFlowOp(Opcode opcode, const Src& src0, const Src& src1, bool test = false) {
    u32 operands_length = src0.GetLength(0b0000) + src1.GetLength(0b0000);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length) | (test ? (1 << 18) : 0));
    src0.Write(code_, true, 0b0000);
    src1.Write(code_, true, 0b0000);
    ++stat_.instruction_count;
  }
  void EmitAtomicOp(Opcode opcode, const Dest& dest, const Src& address, u32 address_components,
                    const Src& value) {
    // Atomic operations require a 0 component memory destination.
    REX_ASSERT(dest.GetMask() == 0);
    u32 address_mask = (1 << address_components) - 1;
    u32 operands_length =
        dest.GetLength() + address.GetLength(address_mask) + value.GetLength(0b0001);
    code_.reserve(code_.size() + 1 + operands_length);
    code_.push_back(OpcodeToken(opcode, operands_length));
    dest.Write(code_);
    address.Write(code_, true, address_mask);
    value.Write(code_, true, 0b0001);
    ++stat_.instruction_count;
    ++stat_.c_interlocked_instructions;
  }

  std::vector<u32>& code_;
  Statistics& stat_;
};

}  // namespace rex::gpu::dxbc
