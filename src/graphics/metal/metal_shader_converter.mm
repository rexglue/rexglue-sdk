/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Static Recompilation Runtime
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 *
 * Metal Shader Converter (MSC) wrapper implementation.
 *
 * Dynamically loads Apple's libmetalirconverter.dylib and uses its C API
 * to convert DXIL bytecode into Metal IR (metallib data).
 *
 * The MSC library is part of Apple's Metal developer tools and must be
 * installed at /usr/local/lib/libmetalirconverter.dylib (or via the
 * metal-shader-converter Homebrew formula).
 */

#include <rex/graphics/metal/metal_shader_converter.h>
#include <rex/logging.h>

#include <dlfcn.h>
#include <mutex>

namespace rex::graphics::metal {

// ============================================================================
// MSC C API type definitions (mirrors metal_irconverter.h)
// ============================================================================
// These are defined here to avoid requiring the MSC header in the public
// interface. They must match the ABI of libmetalirconverter exactly.

// Opaque types
typedef struct IRCompiler IRCompiler;
typedef struct IRObject IRObject;
typedef struct IRRootSignature IRRootSignature;
typedef struct IRMetalLibBinary IRMetalLibBinary;
typedef struct IRError IRError;
typedef struct IRShaderReflection IRShaderReflection;

// Enums
typedef enum {
  IRBytecodeOwnershipNone = 0,
} IRBytecodeOwnership;

typedef enum {
  // Values match the installed libmetalirconverter.dylib (3.0.4+ header).
  // The original MSC 3.0.4 package (Sep 2025) used 0-based values starting
  // with Vertex=0. The updated dylib (Feb 2026) inserted IRShaderStageInvalid
  // at 0 and shifted all stages +1. We must use the values that match whatever
  // dylib is loaded at runtime; see also IRObjectGetMetalIRShaderStage() which
  // lets us auto-detect the compiled stage without hard-coding enum values.
  IRShaderStageInvalid      = 0,   // old API: -1
  IRShaderStageVertex       = 1,   // old API:  0
  IRShaderStageFragment     = 2,   // old API:  1
  IRShaderStageHull         = 3,   // unchanged
  IRShaderStageDomain       = 4,   // unchanged
  IRShaderStageMesh         = 5,   // unchanged
  IRShaderStageAmplification = 6,  // old API: 14
  IRShaderStageGeometry     = 7,   // old API: 13
  IRShaderStageCompute      = 8,   // old API:  2
} IRShaderStage;

typedef enum {
  // Updated to match libmetalirconverter.dylib 3.0.4+ (Feb 2026 header).
  // In the original Sep 2025 package: ForceTextureArray=(1<<0), BoundsCheck=(1<<1).
  // In the updated package: BoundsCheck moved to (1<<0), ForceTextureArray to (1<<8),
  // and 7 new flags occupy bits 1-7.
  IRCompatibilityFlagBoundsCheck          = (1 << 0),  // old API: (1 << 1)
  IRCompatibilityFlagForceTextureArray    = (1 << 8),  // old API: (1 << 0)
} IRCompatibilityFlags;

typedef enum {
  IRInputTopologyUndefined = 0,
  IRInputTopologyPoint = 1,
  IRInputTopologyLine = 2,
  IRInputTopologyTriangle = 3,
  IRInputTopologyLineAdj = 6,
  IRInputTopologyTriangleAdj = 7,
} IRInputTopology;

typedef enum {
  IRShaderVisibilityAll = 0,
  IRShaderVisibilityVertex = 1,
  IRShaderVisibilityHull = 2,
  IRShaderVisibilityDomain = 3,
  IRShaderVisibilityGeometry = 4,
  IRShaderVisibilityPixel = 5,
} IRShaderVisibility;

typedef enum {
  IRDescriptorRangeTypeSRV = 0,
  IRDescriptorRangeTypeUAV = 1,
  IRDescriptorRangeTypeCBV = 2,
  IRDescriptorRangeTypeSampler = 3,
} IRDescriptorRangeType;

typedef enum {
  IRDescriptorRangeFlagNone = 0,
} IRDescriptorRangeFlags;

typedef enum {
  IRRootParameterTypeDescriptorTable = 0,
} IRRootParameterType;

typedef enum {
  IRRootSignatureFlagNone = 0,
} IRRootSignatureFlags;

typedef enum {
  IRRootSignatureVersion_1_1 = 2,
} IRRootSignatureVersion;

typedef enum {
  IRGPUFamilyApple1 = 1001,
  IRGPUFamilyApple7 = 1007,
  IRGPUFamilyMetal3 = 5001,
} IRGPUFamily;

typedef enum {
  // Matches the installed libmetalirconverter.dylib (enum starts at 0).
  IROperatingSystem_macOS          = 0,
  IROperatingSystem_iOS            = 1,
  IROperatingSystem_tvOS           = 2,
  IROperatingSystem_iOSSimulator   = 3,
  // Legacy alias used in older ReXGlue code — DO NOT USE (was wrong value 4).
  IROperatingSystemMacOS = IROperatingSystem_macOS,
} IROperatingSystem;

typedef enum {
  IRReflectionVersion_1_0 = 1,
} IRReflectionVersion;

// Descriptor range
struct IRDescriptorRange1 {
  IRDescriptorRangeType RangeType;
  uint32_t NumDescriptors;
  uint32_t BaseShaderRegister;
  uint32_t RegisterSpace;
  IRDescriptorRangeFlags Flags;
  uint32_t OffsetInDescriptorsFromTableStart;
};

// Descriptor table
struct IRRootDescriptorTable1 {
  uint32_t NumDescriptorRanges;
  const IRDescriptorRange1* pDescriptorRanges;
};

// Root constants (for union sizing)
struct IRRootConstants {
  uint32_t ShaderRegister;
  uint32_t RegisterSpace;
  uint32_t Num32BitValues;
};

// Root descriptor (for union sizing)
struct IRRootDescriptor1 {
  uint32_t ShaderRegister;
  uint32_t RegisterSpace;
  uint32_t Flags;
};

// Root parameter
struct IRRootParameter1 {
  IRRootParameterType ParameterType;
  union {
    IRRootDescriptorTable1 DescriptorTable;
    IRRootConstants Constants;
    IRRootDescriptor1 Descriptor;
  };
  IRShaderVisibility ShaderVisibility;
};

// Root signature descriptor
struct IRRootSignatureDescriptor1 {
  uint32_t NumParameters;
  const IRRootParameter1* pParameters;
  uint32_t NumStaticSamplers;
  const void* pStaticSamplers;
  IRRootSignatureFlags Flags;
};

struct IRVersionedRootSignatureDescriptor {
  IRRootSignatureVersion version;
  union {
    IRRootSignatureDescriptor1 desc_1_0;
    IRRootSignatureDescriptor1 desc_1_1;
  };
};

// Reflection types
// Must match IRVSInfo_1_0 from metal_irconverter.h exactly.
struct IRVersionedVSInfo_1_0 {
  int instance_id_index;
  int vertex_id_index;
  uint32_t vertex_output_size_in_bytes;
  bool needs_draw_params;
  struct {
    const char* name;
    uint8_t attributeIndex;
  }* vertex_inputs;
  size_t num_vertex_inputs;
};

struct IRVersionedVSInfo {
  IRReflectionVersion version;
  IRVersionedVSInfo_1_0 info_1_0;
};

// Must match IRGSInfo_1_0 from metal_irconverter.h exactly.
struct IRVersionedGSInfo_1_0 {
  void* vertex_outputs;  // IRVertexOutputInfo_1_0*
  size_t num_vertex_outputs;
  bool is_passthrough;
  int32_t rt_array_index_record_id;
  int32_t viewport_array_index_record_id;
  uint32_t input_primitive;  // IRInputPrimitive
  uint32_t max_input_primitives_per_mesh_threadgroup;
  uint32_t max_payload_size_in_bytes;
  uint32_t instance_count;
};

struct IRVersionedGSInfo {
  IRReflectionVersion version;
  IRVersionedGSInfo_1_0 info_1_0;
};

struct IRFunctionConstant {
  const char* name;
  uint32_t type;
};

// Must match IRHSInfo_1_0 from metal_irconverter.h exactly.
struct IRVersionedHSInfo_1_0 {
  uint32_t max_patches_per_object_threadgroup;
  uint32_t max_object_threads_per_patch;
  uint32_t patch_constants_size;
  const char* patch_constant_function;
  uint32_t static_payload_size;
  uint32_t payload_size_per_patch;
  uint32_t input_control_point_count;
  uint32_t output_control_point_count;
  uint32_t output_control_point_size;
  uint32_t tessellator_domain;
  uint32_t tessellator_partitioning;
  uint32_t tessellator_output_primitive;
  bool tessellation_type_half;
  float max_tessellation_factor;
};

struct IRVersionedHSInfo {
  IRReflectionVersion version;
  IRVersionedHSInfo_1_0 info_1_0;
};

// Must match IRDSInfo_1_0 from metal_irconverter.h exactly.
struct IRVersionedDSInfo_1_0 {
  uint32_t tessellator_domain;  // IRTessellatorDomain — must be first
  uint32_t max_input_prims_per_mesh_threadgroup;
  uint32_t input_control_point_count;
  uint32_t input_control_point_size;
  uint32_t patch_constants_size;
  bool tessellation_type_half;
};

struct IRVersionedDSInfo {
  IRReflectionVersion version;
  IRVersionedDSInfo_1_0 info_1_0;
};

}  // namespace rex::graphics::metal (temporarily closed for ABI verification)

// ============================================================================
// Compile-time ABI verification against the real MSC header (if installed)
// ============================================================================
// The MSC header must be included at file scope (outside our namespace) to
// avoid type redefinition conflicts. We compare our namespaced struct sizes
// against the real ones from the header.
#if __has_include(<metal_irconverter/metal_irconverter.h>)
#include <metal_irconverter/metal_irconverter.h>
static_assert(sizeof(rex::graphics::metal::IRRootParameter1) == sizeof(::IRRootParameter1),
              "IRRootParameter1 size mismatch with MSC header");
static_assert(sizeof(rex::graphics::metal::IRVersionedRootSignatureDescriptor) ==
                  sizeof(::IRVersionedRootSignatureDescriptor),
              "IRVersionedRootSignatureDescriptor size mismatch with MSC header");
static_assert(sizeof(rex::graphics::metal::IRVersionedVSInfo) == sizeof(::IRVersionedVSInfo),
              "IRVersionedVSInfo size mismatch with MSC header");
static_assert(sizeof(rex::graphics::metal::IRVersionedGSInfo) == sizeof(::IRVersionedGSInfo),
              "IRVersionedGSInfo size mismatch with MSC header");
static_assert(sizeof(rex::graphics::metal::IRVersionedHSInfo) == sizeof(::IRVersionedHSInfo),
              "IRVersionedHSInfo size mismatch with MSC header");
static_assert(sizeof(rex::graphics::metal::IRVersionedDSInfo) == sizeof(::IRVersionedDSInfo),
              "IRVersionedDSInfo size mismatch with MSC header");
#endif

namespace rex::graphics::metal {  // reopened

// ============================================================================
// Function pointer typedefs
// ============================================================================

using PFN_IRCompilerCreate = IRCompiler* (*)();
using PFN_IRCompilerDestroy = void (*)(IRCompiler*);
using PFN_IRCompilerAllocCompileAndLink = IRObject* (*)(
    IRCompiler*, const char*, IRObject*, IRError**);
using PFN_IRCompilerSetGlobalRootSignature = void (*)(IRCompiler*,
                                                       IRRootSignature*);
using PFN_IRCompilerSetCompatibilityFlags = void (*)(IRCompiler*, uint64_t);
using PFN_IRCompilerSetInputTopology = void (*)(IRCompiler*, IRInputTopology);
using PFN_IRCompilerEnableGeometryAndTessellationEmulation = void (*)(
    IRCompiler*, bool);
using PFN_IRCompilerIgnoreRootSignature = void (*)(IRCompiler*, bool);
using PFN_IRCompilerSetFunctionConstantResourceSpace = void (*)(IRCompiler*,
                                                                 uint32_t);
using PFN_IRCompilerSetMinimumGPUFamily = void (*)(IRCompiler*, IRGPUFamily);
using PFN_IRCompilerSetMinimumDeploymentTarget = void (*)(
    IRCompiler*, IROperatingSystem, const char*);
using PFN_IRObjectCreateFromDXIL = IRObject* (*)(const uint8_t*, size_t,
                                                  IRBytecodeOwnership);
using PFN_IRObjectDestroy = void (*)(IRObject*);
using PFN_IRObjectGetMetalLibBinary = bool (*)(IRObject*, IRShaderStage,
                                                IRMetalLibBinary*);
using PFN_IRObjectGetReflection = bool (*)(IRObject*, IRShaderStage,
                                            IRShaderReflection*);
using PFN_IRMetalLibBinaryCreate = IRMetalLibBinary* (*)();
using PFN_IRMetalLibBinaryDestroy = void (*)(IRMetalLibBinary*);
using PFN_IRMetalLibGetBytecodeSize = size_t (*)(IRMetalLibBinary*);
using PFN_IRMetalLibGetBytecode = void (*)(IRMetalLibBinary*, uint8_t*);
using PFN_IRMetalLibSynthesizeStageInFunction = bool (*)(
    IRCompiler*, IRShaderReflection*, const IRVersionedInputLayoutDescriptor*,
    IRMetalLibBinary*);
using PFN_IRRootSignatureCreateFromDescriptor =
    IRRootSignature* (*)(const IRVersionedRootSignatureDescriptor*, IRError**);
using PFN_IRRootSignatureDestroy = void (*)(IRRootSignature*);
using PFN_IRErrorGetPayload = const void* (*)(IRError*);
using PFN_IRErrorDestroy = void (*)(IRError*);
using PFN_IRShaderReflectionCreate = IRShaderReflection* (*)();
using PFN_IRShaderReflectionDestroy = void (*)(IRShaderReflection*);
using PFN_IRShaderReflectionGetEntryPointFunctionName = const char* (*)(
    IRShaderReflection*);
using PFN_IRShaderReflectionCopyVertexInfo = bool (*)(IRShaderReflection*,
                                                       IRReflectionVersion,
                                                       IRVersionedVSInfo*);
using PFN_IRShaderReflectionReleaseVertexInfo = void (*)(IRVersionedVSInfo*);
using PFN_IRShaderReflectionCopyGeometryInfo = bool (*)(IRShaderReflection*,
                                                         IRReflectionVersion,
                                                         IRVersionedGSInfo*);
using PFN_IRShaderReflectionReleaseGeometryInfo = void (*)(IRVersionedGSInfo*);
using PFN_IRShaderReflectionNeedsFunctionConstants = bool (*)(
    IRShaderReflection*);
using PFN_IRShaderReflectionGetFunctionConstantCount = size_t (*)(
    IRShaderReflection*);
using PFN_IRShaderReflectionCopyFunctionConstants = void (*)(
    IRShaderReflection*, IRFunctionConstant*);
using PFN_IRShaderReflectionReleaseFunctionConstants = void (*)(
    IRFunctionConstant*, size_t);
using PFN_IRShaderReflectionCopyHullInfo = bool (*)(IRShaderReflection*,
                                                     IRReflectionVersion,
                                                     IRVersionedHSInfo*);
using PFN_IRShaderReflectionReleaseHullInfo = void (*)(IRVersionedHSInfo*);
using PFN_IRShaderReflectionCopyDomainInfo = bool (*)(IRShaderReflection*,
                                                       IRReflectionVersion,
                                                       IRVersionedDSInfo*);
using PFN_IRShaderReflectionReleaseDomainInfo = void (*)(IRVersionedDSInfo*);
using PFN_IRVersionedRootSignatureDescriptorCopyJSONString = const char* (*)(
    const IRVersionedRootSignatureDescriptor*);
using PFN_IRVersionedRootSignatureDescriptorReleaseString = void (*)(
    const char*);
using PFN_IRInputLayoutDescriptor1CopyJSONString = const char* (*) (const void*);
using PFN_IRInputLayoutDescriptor1ReleaseString = void (*) (const char*);

// ============================================================================
// Helper: typed accessor for function pointers
// ============================================================================

#define MSC_FN(name) reinterpret_cast<PFN_##name>(msc_fn_.name)

// Function-constant register space used by MSC for specialization.
constexpr uint32_t kFunctionConstantRegisterSpace = 2147420894u;

// Search paths for the MSC library.
static const char* const kMSCSearchPaths[] = {
    "libmetalirconverter.dylib",
    "/usr/local/lib/libmetalirconverter.dylib",
    "/opt/homebrew/lib/libmetalirconverter.dylib",
    nullptr,
};

// ============================================================================
// MetalShaderConverter
// ============================================================================

MetalShaderConverter::MetalShaderConverter() = default;

MetalShaderConverter::~MetalShaderConverter() {
  if (cached_root_sig_) {
    if (msc_lib_) {
      DestroyRootSignature(cached_root_sig_);
    }
    cached_root_sig_ = nullptr;
  }
  if (msc_lib_) {
    dlclose(msc_lib_);
    msc_lib_ = nullptr;
  }
}

void MetalShaderConverter::SetMinimumTarget(uint32_t gpu_family, uint32_t os,
                                            const std::string& version) {
  has_minimum_target_ = true;
  minimum_gpu_family_ = gpu_family;
  minimum_os_ = os;
  minimum_os_version_ = version;
}

bool MetalShaderConverter::Initialize() {
  // Try each search path.
  for (const char* const* path = kMSCSearchPaths; *path; ++path) {
    msc_lib_ = dlopen(*path, RTLD_LAZY);
    if (msc_lib_) {
      REXGPU_INFO("MetalShaderConverter: Loaded MSC from {}", *path);
      break;
    }
  }

  if (!msc_lib_) {
    REXGPU_WARN(
        "MetalShaderConverter: libmetalirconverter.dylib not found. "
        "DXIL→Metal conversion will not be available. "
        "Install Apple's Metal Shader Converter tools to enable.");
    is_available_ = false;
    return false;
  }

  // Load all required function pointers.
#define LOAD_MSC_FN(name)                                  \
  msc_fn_.name = dlsym(msc_lib_, #name);                  \
  if (!msc_fn_.name) {                                     \
    REXGPU_ERROR("MetalShaderConverter: Missing symbol: {}", #name); \
    dlclose(msc_lib_);                                     \
    msc_lib_ = nullptr;                                    \
    is_available_ = false;                                 \
    return false;                                          \
  }

  LOAD_MSC_FN(IRCompilerCreate);
  LOAD_MSC_FN(IRCompilerDestroy);
  LOAD_MSC_FN(IRCompilerAllocCompileAndLink);
  LOAD_MSC_FN(IRCompilerSetGlobalRootSignature);
  LOAD_MSC_FN(IRCompilerSetCompatibilityFlags);
  LOAD_MSC_FN(IRCompilerSetInputTopology);
  LOAD_MSC_FN(IRCompilerEnableGeometryAndTessellationEmulation);
  LOAD_MSC_FN(IRCompilerIgnoreRootSignature);
  LOAD_MSC_FN(IRCompilerSetFunctionConstantResourceSpace);
  LOAD_MSC_FN(IRCompilerSetMinimumGPUFamily);
  LOAD_MSC_FN(IRCompilerSetMinimumDeploymentTarget);
  LOAD_MSC_FN(IRObjectCreateFromDXIL);
  LOAD_MSC_FN(IRObjectDestroy);
  LOAD_MSC_FN(IRObjectGetMetalLibBinary);
  LOAD_MSC_FN(IRObjectGetReflection);
  LOAD_MSC_FN(IRMetalLibBinaryCreate);
  LOAD_MSC_FN(IRMetalLibBinaryDestroy);
  LOAD_MSC_FN(IRMetalLibGetBytecodeSize);
  LOAD_MSC_FN(IRMetalLibGetBytecode);
  LOAD_MSC_FN(IRRootSignatureCreateFromDescriptor);
  LOAD_MSC_FN(IRRootSignatureDestroy);
  LOAD_MSC_FN(IRErrorGetPayload);
  LOAD_MSC_FN(IRErrorDestroy);
  LOAD_MSC_FN(IRShaderReflectionCreate);
  LOAD_MSC_FN(IRShaderReflectionDestroy);
  LOAD_MSC_FN(IRShaderReflectionGetEntryPointFunctionName);
  LOAD_MSC_FN(IRShaderReflectionCopyVertexInfo);
  LOAD_MSC_FN(IRShaderReflectionReleaseVertexInfo);
  LOAD_MSC_FN(IRShaderReflectionCopyGeometryInfo);
  LOAD_MSC_FN(IRShaderReflectionReleaseGeometryInfo);
  LOAD_MSC_FN(IRShaderReflectionNeedsFunctionConstants);
  LOAD_MSC_FN(IRShaderReflectionGetFunctionConstantCount);
  LOAD_MSC_FN(IRShaderReflectionCopyFunctionConstants);
  LOAD_MSC_FN(IRShaderReflectionReleaseFunctionConstants);
  LOAD_MSC_FN(IRShaderReflectionCopyHullInfo);
  LOAD_MSC_FN(IRShaderReflectionReleaseHullInfo);
  LOAD_MSC_FN(IRShaderReflectionCopyDomainInfo);
  LOAD_MSC_FN(IRShaderReflectionReleaseDomainInfo);
  LOAD_MSC_FN(IRVersionedRootSignatureDescriptorCopyJSONString);
  LOAD_MSC_FN(IRVersionedRootSignatureDescriptorReleaseString);

  // Optional symbols used for stage-in synthesis.
  msc_fn_.IRMetalLibSynthesizeStageInFunction =
      dlsym(msc_lib_, "IRMetalLibSynthesizeStageInFunction");
  msc_fn_.IRInputLayoutDescriptor1CopyJSONString =
      dlsym(msc_lib_, "IRInputLayoutDescriptor1CopyJSONString");
  msc_fn_.IRInputLayoutDescriptor1ReleaseString =
      dlsym(msc_lib_, "IRInputLayoutDescriptor1ReleaseString");
  // Optional: present in updated MSC 3.0.4+ (Feb 2026). Returns the actual
  // compiled IRShaderStage from a metalObject so we never hard-code enum
  // values when calling IRObjectGetMetalLibBinary. Falls back to the static
  // ir_stage when absent (older installs).
  msc_fn_.IRObjectGetMetalIRShaderStage =
      dlsym(msc_lib_, "IRObjectGetMetalIRShaderStage");
  if (msc_fn_.IRObjectGetMetalIRShaderStage) {
    REXGPU_DEBUG("MetalShaderConverter: IRObjectGetMetalIRShaderStage available "
                 "(stage auto-detection enabled)");
  } else {
    REXGPU_DEBUG("MetalShaderConverter: IRObjectGetMetalIRShaderStage not found "
                 "(older dylib — using static enum stage values)");
  }

#undef LOAD_MSC_FN

  // Test that we can create a compiler.
  IRCompiler* test_compiler = MSC_FN(IRCompilerCreate)();
  if (!test_compiler) {
    REXGPU_ERROR("MetalShaderConverter: Failed to create test IR compiler");
    is_available_ = false;
    return false;
  }
  MSC_FN(IRCompilerDestroy)(test_compiler);

  // Pre-create the cached root signature during initialization.
  // This eliminates a data race if multiple threads call ConvertWithStageEx()
  // concurrently on first use (previously, GetCachedRootSignature() lazily
  // initialized cached_root_sig_ without synchronization).
  cached_root_sig_ = CreateXbox360RootSignature(MetalShaderStage::kVertex, true);
  if (!cached_root_sig_) {
    REXGPU_WARN("MetalShaderConverter: Failed to pre-create root signature; "
                "will attempt lazy creation on first use");
  }

  REXGPU_INFO("MetalShaderConverter: Initialized successfully");
  is_available_ = true;
  return true;
}

// ============================================================================
// Xbox 360 root signature
// ============================================================================

void* MetalShaderConverter::CreateXbox360RootSignature(
    MetalShaderStage stage, bool force_all_visibility) {
  IRShaderVisibility visibility = IRShaderVisibilityAll;
  if (!force_all_visibility) {
    switch (stage) {
      case MetalShaderStage::kVertex:
        visibility = IRShaderVisibilityVertex;
        break;
      case MetalShaderStage::kFragment:
        visibility = IRShaderVisibilityPixel;
        break;
      case MetalShaderStage::kHull:
        visibility = IRShaderVisibilityHull;
        break;
      case MetalShaderStage::kDomain:
        visibility = IRShaderVisibilityDomain;
        break;
      default:
        visibility = IRShaderVisibilityAll;
        break;
    }
  }

  // Create descriptor ranges for Xbox 360 shader resources.
  // This matches the layout used by Xenia's D3D12 backend.
  IRDescriptorRange1 ranges[20] = {};
  int rangeIdx = 0;

  // SRVs in spaces 0-3 (1025 descriptors = 1024 + 1 padding).
  for (int space = 0; space < 4; space++) {
    ranges[rangeIdx].RangeType = IRDescriptorRangeTypeSRV;
    ranges[rangeIdx].NumDescriptors = 1025;
    ranges[rangeIdx].BaseShaderRegister = 0;
    ranges[rangeIdx].RegisterSpace = space;
    ranges[rangeIdx].Flags = IRDescriptorRangeFlagNone;
    ranges[rangeIdx].OffsetInDescriptorsFromTableStart = 0;
    rangeIdx++;
  }

  // SRV in space 10 for hull shaders.
  ranges[rangeIdx].RangeType = IRDescriptorRangeTypeSRV;
  ranges[rangeIdx].NumDescriptors = 1025;
  ranges[rangeIdx].BaseShaderRegister = 0;
  ranges[rangeIdx].RegisterSpace = 10;
  ranges[rangeIdx].Flags = IRDescriptorRangeFlagNone;
  ranges[rangeIdx].OffsetInDescriptorsFromTableStart = 0;
  rangeIdx++;

  // UAVs in spaces 0-3.
  for (int space = 0; space < 4; space++) {
    ranges[rangeIdx].RangeType = IRDescriptorRangeTypeUAV;
    ranges[rangeIdx].NumDescriptors = 1025;
    ranges[rangeIdx].BaseShaderRegister = 0;
    ranges[rangeIdx].RegisterSpace = space;
    ranges[rangeIdx].Flags = IRDescriptorRangeFlagNone;
    ranges[rangeIdx].OffsetInDescriptorsFromTableStart = 0;
    rangeIdx++;
  }

  // Samplers in space 0 (257 = 256 + 1 padding).
  ranges[rangeIdx].RangeType = IRDescriptorRangeTypeSampler;
  ranges[rangeIdx].NumDescriptors = 257;
  ranges[rangeIdx].BaseShaderRegister = 0;
  ranges[rangeIdx].RegisterSpace = 0;
  ranges[rangeIdx].Flags = IRDescriptorRangeFlagNone;
  ranges[rangeIdx].OffsetInDescriptorsFromTableStart = 0;
  rangeIdx++;

  // CBVs in spaces 0-3.
  // Space 0 has 5 CBVs: b0=system constants, b1=float constants,
  // b2=bool/loop constants, b3=fetch constants, b4=descriptor indices.
  for (int space = 0; space < 4; space++) {
    ranges[rangeIdx].RangeType = IRDescriptorRangeTypeCBV;
    ranges[rangeIdx].NumDescriptors = (space == 0) ? 5 : 1;
    ranges[rangeIdx].BaseShaderRegister = 0;
    ranges[rangeIdx].RegisterSpace = space;
    ranges[rangeIdx].Flags = IRDescriptorRangeFlagNone;
    ranges[rangeIdx].OffsetInDescriptorsFromTableStart = 0;
    rangeIdx++;
  }

  // Function-constant CBV space for MSC specialization.
  ranges[rangeIdx].RangeType = IRDescriptorRangeTypeCBV;
  ranges[rangeIdx].NumDescriptors = 1;
  ranges[rangeIdx].BaseShaderRegister = 0;
  ranges[rangeIdx].RegisterSpace = kFunctionConstantRegisterSpace;
  ranges[rangeIdx].Flags = IRDescriptorRangeFlagNone;
  ranges[rangeIdx].OffsetInDescriptorsFromTableStart = 0;
  rangeIdx++;

  // Build descriptor tables and root parameters.
  IRRootDescriptorTable1 tables[20] = {};
  IRRootParameter1 params[20] = {};

  for (int i = 0; i < rangeIdx; i++) {
    tables[i].NumDescriptorRanges = 1;
    tables[i].pDescriptorRanges = &ranges[i];
    params[i].ParameterType = IRRootParameterTypeDescriptorTable;
    params[i].DescriptorTable = tables[i];
    params[i].ShaderVisibility = visibility;
  }

  IRRootSignatureDescriptor1 desc = {};
  desc.NumParameters = rangeIdx;
  desc.pParameters = params;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = IRRootSignatureFlagNone;

  IRVersionedRootSignatureDescriptor versionedDesc = {};
  versionedDesc.version = IRRootSignatureVersion_1_1;
  versionedDesc.desc_1_1 = desc;

  // Log root signature JSON once for debugging.
  static std::once_flag logged_root_sig_flag;
  std::call_once(logged_root_sig_flag, [&]() {
    const char* json =
        MSC_FN(IRVersionedRootSignatureDescriptorCopyJSONString)(
            &versionedDesc);
    if (json) {
      REXGPU_DEBUG("MetalShaderConverter: Xbox 360 root signature: {}", json);
      MSC_FN(IRVersionedRootSignatureDescriptorReleaseString)(json);
    }
  });

  IRError* error = nullptr;
  IRRootSignature* rootSig =
      MSC_FN(IRRootSignatureCreateFromDescriptor)(&versionedDesc, &error);

  if (error) {
    const char* errMsg =
        static_cast<const char*>(MSC_FN(IRErrorGetPayload)(error));
    REXGPU_ERROR("MetalShaderConverter: Failed to create root signature: {}",
                 errMsg ? errMsg : "unknown error");
    MSC_FN(IRErrorDestroy)(error);
    return nullptr;
  }

  return rootSig;
}

void MetalShaderConverter::DestroyRootSignature(void* root_sig) {
  if (root_sig) {
    MSC_FN(IRRootSignatureDestroy)(static_cast<IRRootSignature*>(root_sig));
  }
}

void* MetalShaderConverter::GetCachedRootSignature() {
  // Root signature is eagerly created in Initialize(). This fallback
  // handles the unlikely case where Initialize() pre-creation failed.
  if (!cached_root_sig_) {
    REXGPU_WARN("MetalShaderConverter: Lazy root signature creation "
                "(Initialize pre-creation failed)");
    cached_root_sig_ = CreateXbox360RootSignature(MetalShaderStage::kVertex, true);
  }
  return cached_root_sig_;
}

// ============================================================================
// Conversion
// ============================================================================

bool MetalShaderConverter::Convert(xenos::ShaderType shader_type,
                                   const std::vector<uint8_t>& dxil_data,
                                   MetalShaderConversionResult& result) {
  MetalShaderStage stage;
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      stage = MetalShaderStage::kVertex;
      break;
    case xenos::ShaderType::kPixel:
      stage = MetalShaderStage::kFragment;
      break;
    default:
      result.success = false;
      result.error_message = "Unsupported shader type";
      return false;
  }
  return ConvertWithStage(stage, dxil_data, result);
}

bool MetalShaderConverter::ConvertWithStage(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil_data,
    MetalShaderConversionResult& result) {
  return ConvertWithStageEx(stage, dxil_data, result, nullptr,
                            nullptr, nullptr, false,
                            IRInputTopologyUndefined);
}

bool MetalShaderConverter::ConvertWithStageEx(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil_data,
    MetalShaderConversionResult& result, MetalShaderReflectionInfo* reflection,
    const IRVersionedInputLayoutDescriptor* input_layout,
    std::vector<uint8_t>* stage_in_metallib,
    bool enable_geometry_emulation, int input_topology) {
  if (!is_available_) {
    result.success = false;
    result.error_message = "MetalShaderConverter not initialized";
    return false;
  }

  if (dxil_data.empty()) {
    result.success = false;
    result.error_message = "Empty DXIL data";
    return false;
  }

  // Create DXIL object from input data.
  IRObject* dxilObject = MSC_FN(IRObjectCreateFromDXIL)(
      dxil_data.data(), dxil_data.size(), IRBytecodeOwnershipNone);
  if (!dxilObject) {
    result.success = false;
    result.error_message = "Failed to create DXIL object";
    return false;
  }

  // Create compiler.
  IRCompiler* compiler = MSC_FN(IRCompilerCreate)();
  if (!compiler) {
    MSC_FN(IRObjectDestroy)(dxilObject);
    result.success = false;
    result.error_message = "Failed to create IR compiler";
    return false;
  }

  // Set compatibility flags:
  // - ForceTextureArray: Xenia's DXBC translator generates texture2d_array
  // - BoundsCheck: Safety for out-of-bounds resource access
  MSC_FN(IRCompilerSetCompatibilityFlags)(
      compiler,
      static_cast<uint64_t>(IRCompatibilityFlagForceTextureArray |
                            IRCompatibilityFlagBoundsCheck));

  if (input_topology != IRInputTopologyUndefined) {
    MSC_FN(IRCompilerSetInputTopology)(
        compiler, static_cast<IRInputTopology>(input_topology));
  }
  if (enable_geometry_emulation) {
    MSC_FN(IRCompilerEnableGeometryAndTessellationEmulation)(compiler, true);
  }

  // Ignore embedded root signatures in DXIL; we provide our own.
  MSC_FN(IRCompilerIgnoreRootSignature)(compiler, true);

  // Enable function-constant register space for MSC specialization.
  MSC_FN(IRCompilerSetFunctionConstantResourceSpace)(
      compiler, kFunctionConstantRegisterSpace);

  if (has_minimum_target_) {
    MSC_FN(IRCompilerSetMinimumGPUFamily)(
        compiler, static_cast<IRGPUFamily>(minimum_gpu_family_));
    MSC_FN(IRCompilerSetMinimumDeploymentTarget)(
        compiler, static_cast<IROperatingSystem>(minimum_os_),
        minimum_os_version_.c_str());
  }

  // Get cached Xbox 360 root signature (all-visibility, shared across calls).
  IRRootSignature* rootSig =
      static_cast<IRRootSignature*>(GetCachedRootSignature());
  if (!rootSig) {
    MSC_FN(IRCompilerDestroy)(compiler);
    MSC_FN(IRObjectDestroy)(dxilObject);
    result.success = false;
    result.error_message = "Failed to create root signature";
    return false;
  }
  MSC_FN(IRCompilerSetGlobalRootSignature)(compiler, rootSig);

  // Compile DXIL to Metal.
  IRError* error = nullptr;
  IRObject* metalObject =
      MSC_FN(IRCompilerAllocCompileAndLink)(compiler, nullptr, dxilObject,
                                            &error);

  if (error) {
    const char* errMsg =
        static_cast<const char*>(MSC_FN(IRErrorGetPayload)(error));
    result.success = false;
    result.error_message = std::string("MSC compilation failed: ") +
                           (errMsg ? errMsg : "unknown error");
    REXGPU_ERROR("MetalShaderConverter: {}", result.error_message);
    MSC_FN(IRErrorDestroy)(error);
    if (metalObject) MSC_FN(IRObjectDestroy)(metalObject);
    MSC_FN(IRCompilerDestroy)(compiler);
    MSC_FN(IRObjectDestroy)(dxilObject);
    return false;
  }

  if (!metalObject) {
    result.success = false;
    result.error_message = "MSC returned null object without error";
    MSC_FN(IRCompilerDestroy)(compiler);
    MSC_FN(IRObjectDestroy)(dxilObject);
    return false;
  }

  // Extract metallib binary for the appropriate stage.
  auto extract_metallib = [&](IRShaderStage ir_stage,
                              std::vector<uint8_t>& out_bytes,
                              size_t* out_size) -> bool {
    IRMetalLibBinary* metallib = MSC_FN(IRMetalLibBinaryCreate)();
    if (!metallib) {
      if (out_size) *out_size = 0;
      return false;
    }
    bool ok = MSC_FN(IRObjectGetMetalLibBinary)(metalObject, ir_stage,
                                                metallib);
    size_t metallib_size = MSC_FN(IRMetalLibGetBytecodeSize)(metallib);
    if (!ok || metallib_size == 0) {
      MSC_FN(IRMetalLibBinaryDestroy)(metallib);
      if (out_size) *out_size = 0;
      return false;
    }
    out_bytes.resize(metallib_size);
    MSC_FN(IRMetalLibGetBytecode)(metallib, out_bytes.data());
    MSC_FN(IRMetalLibBinaryDestroy)(metallib);
    if (out_size) *out_size = metallib_size;
    return true;
  };

  // Determine which IRShaderStage to pass to IRObjectGetMetalLibBinary.
  //
  // Option A (preferred): IRObjectGetMetalIRShaderStage() — present in the
  // updated MSC dylib (Feb 2026, 3.0.4+). Queries the compiled metalObject
  // directly so we never depend on hard-coded enum values that may shift
  // between dylib versions. This is how we fixed the empty-metallib bug
  // caused by IRShaderStageVertex shifting from 0 → 1 in the new API.
  //
  // Option B (fallback): static switch — used when the dylib is older and
  // IRObjectGetMetalIRShaderStage is not exported. Values must match the
  // installed dylib's enum; see the IRShaderStage typedef above.
  IRShaderStage ir_stage = IRShaderStageInvalid;

  if (stage != MetalShaderStage::kGeometry &&
      msc_fn_.IRObjectGetMetalIRShaderStage) {
    // Auto-detect: ask the metalObject which stage it compiled to.
    using PFN_GetStage = IRShaderStage (*)(const IRObject*);
    ir_stage = reinterpret_cast<PFN_GetStage>(
        msc_fn_.IRObjectGetMetalIRShaderStage)(metalObject);
  } else {
    // Static fallback — enum values must match the installed dylib.
    switch (stage) {
      case MetalShaderStage::kVertex:
        ir_stage = IRShaderStageVertex;
        break;
      case MetalShaderStage::kFragment:
        ir_stage = IRShaderStageFragment;
        break;
      case MetalShaderStage::kCompute:
        ir_stage = IRShaderStageCompute;
        break;
      case MetalShaderStage::kHull:
        ir_stage = IRShaderStageHull;
        break;
      case MetalShaderStage::kDomain:
        ir_stage = IRShaderStageDomain;
        break;
      case MetalShaderStage::kGeometry:
        // Leave IRShaderStageInvalid here; kGeometry uses the dedicated
        // mesh/geometry extraction path below which passes IRShaderStageMesh /
        // IRShaderStageGeometry explicitly.
        break;
      default:
        ir_stage = IRShaderStageInvalid;
        break;
    }
  }

  result.has_mesh_stage = false;
  result.has_geometry_stage = false;
  size_t stage_size = 0;

  if (stage == MetalShaderStage::kGeometry) {
    std::vector<uint8_t> mesh_bytes;
    std::vector<uint8_t> geom_bytes;
    result.has_mesh_stage =
        extract_metallib(IRShaderStageMesh, mesh_bytes, nullptr);
    result.has_geometry_stage =
        extract_metallib(IRShaderStageGeometry, geom_bytes, nullptr);
    if (result.has_mesh_stage) {
      result.metallib_data = std::move(mesh_bytes);
      ir_stage = IRShaderStageMesh;
    } else if (result.has_geometry_stage) {
      result.metallib_data = std::move(geom_bytes);
      ir_stage = IRShaderStageGeometry;
    }
  } else if (ir_stage != IRShaderStageInvalid) {
    extract_metallib(ir_stage, result.metallib_data, &stage_size);
  }

  if (result.metallib_data.empty()) {
    result.success = false;
    result.error_message = "Generated MetalLib has zero size";
    REXGPU_ERROR(
        "MetalShaderConverter: empty metallib (ir_stage={}, "
        "geom_emulation={}, mesh_ok={}, geom_ok={})",
        int(ir_stage), enable_geometry_emulation, result.has_mesh_stage,
        result.has_geometry_stage);
    MSC_FN(IRObjectDestroy)(metalObject);
    MSC_FN(IRCompilerDestroy)(compiler);
    MSC_FN(IRObjectDestroy)(dxilObject);
    return false;
  }

  // Extract reflection info.
  IRShaderReflection* shader_reflection = MSC_FN(IRShaderReflectionCreate)();
  if (shader_reflection && ir_stage != IRShaderStageInvalid) {
    if (MSC_FN(IRObjectGetReflection)(metalObject, ir_stage,
                                      shader_reflection)) {
      const char* entry_name =
          MSC_FN(IRShaderReflectionGetEntryPointFunctionName)(
              shader_reflection);
      if (entry_name) {
        result.function_name = entry_name;
      }

      if (reflection) {
        reflection->vertex_inputs.clear();
        reflection->function_constants.clear();
        reflection->vertex_output_size_in_bytes = 0;
        reflection->vertex_input_count = 0;
        reflection->gs_max_input_primitives_per_mesh_threadgroup = 0;
        reflection->has_hull_info = false;
        reflection->has_domain_info = false;

        if (ir_stage == IRShaderStageVertex) {
          IRVersionedVSInfo vs_info = {};
          vs_info.version = IRReflectionVersion_1_0;
          if (MSC_FN(IRShaderReflectionCopyVertexInfo)(
                  shader_reflection, IRReflectionVersion_1_0, &vs_info)) {
            reflection->vertex_output_size_in_bytes =
                vs_info.info_1_0.vertex_output_size_in_bytes;
            reflection->vertex_input_count =
                static_cast<uint32_t>(vs_info.info_1_0.num_vertex_inputs);
            reflection->vertex_inputs.reserve(
                vs_info.info_1_0.num_vertex_inputs);
            for (size_t i = 0; i < vs_info.info_1_0.num_vertex_inputs; ++i) {
              MetalShaderReflectionInput out;
              out.name = vs_info.info_1_0.vertex_inputs[i].name
                             ? vs_info.info_1_0.vertex_inputs[i].name
                             : "";
              out.attribute_index =
                  vs_info.info_1_0.vertex_inputs[i].attributeIndex;
              reflection->vertex_inputs.push_back(std::move(out));
            }
            MSC_FN(IRShaderReflectionReleaseVertexInfo)(&vs_info);
          }
        } else if (ir_stage == IRShaderStageGeometry ||
                   ir_stage == IRShaderStageMesh) {
          IRVersionedGSInfo gs_info = {};
          gs_info.version = IRReflectionVersion_1_0;
          if (MSC_FN(IRShaderReflectionCopyGeometryInfo)(
                  shader_reflection, IRReflectionVersion_1_0, &gs_info)) {
            reflection->gs_max_input_primitives_per_mesh_threadgroup =
                gs_info.info_1_0.max_input_primitives_per_mesh_threadgroup;
            MSC_FN(IRShaderReflectionReleaseGeometryInfo)(&gs_info);
          }
        }

        // Function constants.
        if (MSC_FN(IRShaderReflectionNeedsFunctionConstants)(
                shader_reflection)) {
          size_t constant_count =
              MSC_FN(IRShaderReflectionGetFunctionConstantCount)(
                  shader_reflection);
          if (constant_count) {
            std::vector<IRFunctionConstant> constants(constant_count);
            MSC_FN(IRShaderReflectionCopyFunctionConstants)(
                shader_reflection, constants.data());
            reflection->function_constants.reserve(constant_count);
            for (const auto& constant : constants) {
              MetalShaderFunctionConstant out;
              out.name = constant.name ? constant.name : "";
              out.type = constant.type;
              reflection->function_constants.push_back(std::move(out));
            }
            MSC_FN(IRShaderReflectionReleaseFunctionConstants)(
                constants.data(), constant_count);
          }
        }

        // Hull shader reflection.
        if (ir_stage == IRShaderStageHull) {
          IRVersionedHSInfo hs_info = {};
          hs_info.version = IRReflectionVersion_1_0;
          if (MSC_FN(IRShaderReflectionCopyHullInfo)(
                  shader_reflection, IRReflectionVersion_1_0, &hs_info)) {
            reflection->has_hull_info = true;
            reflection->hs_max_patches_per_object_threadgroup =
                hs_info.info_1_0.max_patches_per_object_threadgroup;
            reflection->hs_max_object_threads_per_patch =
                hs_info.info_1_0.max_object_threads_per_patch;
            reflection->hs_patch_constants_size =
                hs_info.info_1_0.patch_constants_size;
            reflection->hs_input_control_point_count =
                hs_info.info_1_0.input_control_point_count;
            reflection->hs_output_control_point_count =
                hs_info.info_1_0.output_control_point_count;
            reflection->hs_output_control_point_size =
                hs_info.info_1_0.output_control_point_size;
            reflection->hs_tessellator_domain =
                hs_info.info_1_0.tessellator_domain;
            reflection->hs_tessellator_partitioning =
                hs_info.info_1_0.tessellator_partitioning;
            reflection->hs_tessellator_output_primitive =
                hs_info.info_1_0.tessellator_output_primitive;
            reflection->hs_tessellation_type_half =
                hs_info.info_1_0.tessellation_type_half;
            reflection->hs_max_tessellation_factor =
                hs_info.info_1_0.max_tessellation_factor;
            MSC_FN(IRShaderReflectionReleaseHullInfo)(&hs_info);
          }
        }

        // Domain shader reflection.
        if (ir_stage == IRShaderStageDomain) {
          IRVersionedDSInfo ds_info = {};
          ds_info.version = IRReflectionVersion_1_0;
          if (MSC_FN(IRShaderReflectionCopyDomainInfo)(
                  shader_reflection, IRReflectionVersion_1_0, &ds_info)) {
            reflection->has_domain_info = true;
            reflection->ds_tessellator_domain =
                ds_info.info_1_0.tessellator_domain;
            reflection->ds_max_input_prims_per_mesh_threadgroup =
                ds_info.info_1_0.max_input_prims_per_mesh_threadgroup;
            reflection->ds_input_control_point_count =
                ds_info.info_1_0.input_control_point_count;
            reflection->ds_input_control_point_size =
                ds_info.info_1_0.input_control_point_size;
            reflection->ds_patch_constants_size =
                ds_info.info_1_0.patch_constants_size;
            reflection->ds_tessellation_type_half =
                ds_info.info_1_0.tessellation_type_half;
            MSC_FN(IRShaderReflectionReleaseDomainInfo)(&ds_info);
          }
        }
      }
    }
  }

  if (stage == MetalShaderStage::kVertex && stage_in_metallib) {
    stage_in_metallib->clear();
  }
  if (stage == MetalShaderStage::kVertex && input_layout && stage_in_metallib &&
      shader_reflection && msc_fn_.IRMetalLibSynthesizeStageInFunction) {
    IRMetalLibBinary* stage_in_lib = MSC_FN(IRMetalLibBinaryCreate)();
    if (stage_in_lib) {
      if (MSC_FN(IRMetalLibSynthesizeStageInFunction)(
              compiler, shader_reflection, input_layout, stage_in_lib)) {
        size_t stage_in_size = MSC_FN(IRMetalLibGetBytecodeSize)(stage_in_lib);
        if (stage_in_size) {
          stage_in_metallib->resize(stage_in_size);
          MSC_FN(IRMetalLibGetBytecode)(stage_in_lib,
                                        stage_in_metallib->data());
        }
      }
      MSC_FN(IRMetalLibBinaryDestroy)(stage_in_lib);
    }
  }

  // Default function names if reflection didn't provide one.
  if (result.function_name.empty()) {
    switch (stage) {
      case MetalShaderStage::kVertex:
        result.function_name = "vertexMain";
        break;
      case MetalShaderStage::kFragment:
        result.function_name = "fragmentMain";
        break;
      case MetalShaderStage::kCompute:
        result.function_name = "computeMain";
        break;
      default:
        result.function_name = "main";
        break;
    }
  }

  if (shader_reflection) {
    MSC_FN(IRShaderReflectionDestroy)(shader_reflection);
  }

  REXGPU_DEBUG(
      "MetalShaderConverter: Converted {} bytes DXIL → {} bytes MetalLib "
      "(function: {})",
      dxil_data.size(), result.metallib_data.size(), result.function_name);

  // Cleanup (rootSig is cached, do not destroy here).
  MSC_FN(IRObjectDestroy)(metalObject);
  MSC_FN(IRCompilerDestroy)(compiler);
  MSC_FN(IRObjectDestroy)(dxilObject);

  result.success = true;
  return true;
}

#undef MSC_FN

}  // namespace rex::graphics::metal
