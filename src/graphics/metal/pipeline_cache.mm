/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 5: Full pipeline state caching + shader compilation
 */

#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>

#include <rex/graphics/metal/pipeline_cache.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/metal/dxbc_to_dxil_converter.h>
#include <rex/graphics/metal/metal_shader_converter.h>
#include <rex/graphics/metal/shader_cache.h>
#include <rex/graphics/flags.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/logging.h>

namespace rex::graphics::metal {

// ============================================================================
// Fallback Metal shaders
// ============================================================================
// These provide a basic passthrough rendering path so the emulator
// shows *something* before full Xenos shader translation is complete.
// The vertex shader passes through pre-transformed NDC positions.
// The fragment shader outputs the vertex color or a flat magenta if
// no color attribute is present, making untranslated draws visible.
//
// Additionally includes a compute kernel for EDRAM → texture resolve.
static const char* kFallbackShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

// Function constant for texture presence.
// Must be declared before any function that references it.
constant bool has_texture [[function_constant(0)]];

// =======================================================
// Fallback vertex shader
// =======================================================
struct FallbackVertexIn {
  float4 position [[attribute(0)]];
  float4 color    [[attribute(1)]];
  float2 texcoord [[attribute(2)]];
};

struct FallbackVertexOut {
  float4 position [[position]];
  float4 color;
  float2 texcoord;
};

struct FallbackUniforms {
  float4 ndc_scale;    // xyz = scale, w = unused
  float4 ndc_offset;   // xyz = offset, w = unused
  float4 point_size;   // x = size_x, y = size_y, z = min, w = max
  uint   vertex_base;
};

vertex FallbackVertexOut fallback_vertex(
    FallbackVertexIn in [[stage_in]],
    constant FallbackUniforms& uniforms [[buffer(1)]],
    uint vid [[vertex_id]]) {
  FallbackVertexOut out;
  // Apply NDC transform.
  float4 pos = in.position;
  pos.xyz = pos.xyz * uniforms.ndc_scale.xyz + uniforms.ndc_offset.xyz;
  out.position = pos;
  out.color = in.color;
  out.texcoord = in.texcoord;
  return out;
}

// =======================================================
// Fallback fragment shader
// =======================================================
struct FallbackFragmentOut {
  float4 color0 [[color(0)]];
};

fragment FallbackFragmentOut fallback_fragment(
    FallbackVertexOut in [[stage_in]],
    texture2d<float> tex0 [[texture(0), function_constant(has_texture)]],
    sampler samp0 [[sampler(0), function_constant(has_texture)]]) {
  FallbackFragmentOut out;
  float4 base_color = in.color;
  // If all color channels are zero, show magenta so it's visible.
  if (base_color.r == 0.0 && base_color.g == 0.0 &&
      base_color.b == 0.0 && base_color.a == 0.0) {
    base_color = float4(1.0, 0.0, 1.0, 1.0);
  }
  out.color0 = base_color;
  return out;
}

// =======================================================
// EDRAM resolve compute kernel
// =======================================================
// Copies a rectangular region from the EDRAM buffer to a 2D texture.
// Handles 32bpp and 64bpp formats. Runs one thread per pixel.
struct ResolveParams {
  uint edram_base;       // Byte offset into EDRAM buffer
  uint edram_pitch;      // Pitch in bytes per row in EDRAM
  uint src_x;            // Source X offset in pixels
  uint src_y;            // Source Y offset in pixels
  uint width;            // Region width in pixels
  uint height;           // Region height in pixels
  uint bytes_per_pixel;  // 4 or 8
  uint format;           // Output format selector
};

kernel void resolve_edram_to_texture(
    device const uint8_t* edram [[buffer(0)]],
    texture2d<float, access::write> output [[texture(0)]],
    constant ResolveParams& params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) return;

  uint src_x = params.src_x + gid.x;
  uint src_y = params.src_y + gid.y;
  uint src_offset = params.edram_base + src_y * params.edram_pitch +
                    src_x * params.bytes_per_pixel;

  float4 color;
  if (params.bytes_per_pixel == 4) {
    // RGBA8 or similar 32bpp format.
    uint pixel = *reinterpret_cast<device const uint*>(edram + src_offset);
    color = float4(
        float((pixel >> 0) & 0xFF) / 255.0,
        float((pixel >> 8) & 0xFF) / 255.0,
        float((pixel >> 16) & 0xFF) / 255.0,
        float((pixel >> 24) & 0xFF) / 255.0);
  } else {
    // 64bpp (e.g., RGBA16F) - read as two uint32s.
    uint2 data = *reinterpret_cast<device const uint2*>(edram + src_offset);
    // Interpret as half4.
    half4 h;
    h.x = as_type<half>(ushort(data.x & 0xFFFF));
    h.y = as_type<half>(ushort(data.x >> 16));
    h.z = as_type<half>(ushort(data.y & 0xFFFF));
    h.w = as_type<half>(ushort(data.y >> 16));
    color = float4(h);
  }

  output.write(color, gid);
}

// =======================================================
// Render target -> EDRAM store compute kernel
// =======================================================
struct StoreParams {
  uint edram_base;       // Byte offset into EDRAM buffer
  uint edram_pitch;      // Pitch in bytes per row in EDRAM
  uint width;            // Region width in pixels
  uint height;           // Region height in pixels
  uint bytes_per_pixel;  // 4 or 8
};

kernel void store_texture_to_edram(
    texture2d<float, access::read> source [[texture(0)]],
    device uint8_t* edram [[buffer(0)]],
    constant StoreParams& params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) return;

  float4 color = source.read(gid);
  uint dst_offset = params.edram_base + gid.y * params.edram_pitch +
                    gid.x * params.bytes_per_pixel;

  if (params.bytes_per_pixel == 4) {
    uint r = uint(clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint g = uint(clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint b = uint(clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint a = uint(clamp(color.a, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint packed = (a << 24) | (b << 16) | (g << 8) | r;
    *reinterpret_cast<device uint*>(edram + dst_offset) = packed;
  } else {
    half4 h = half4(color);
    uint2 packed;
    packed.x = uint(as_type<ushort>(h.x)) | (uint(as_type<ushort>(h.y)) << 16);
    packed.y = uint(as_type<ushort>(h.z)) | (uint(as_type<ushort>(h.w)) << 16);
    *reinterpret_cast<device uint2*>(edram + dst_offset) = packed;
  }
}
)";

static MTLVertexFormat GetMetalVertexFormatForFetch(
    const Shader::VertexBinding::Attribute& attr) {
  const auto& fetch = attr.fetch_instr;
  switch (fetch.attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
      if (fetch.attributes.is_integer) {
        return fetch.attributes.is_signed ? MTLVertexFormatChar4
                                          : MTLVertexFormatUChar4;
      }
      return fetch.attributes.is_signed ? MTLVertexFormatChar4Normalized
                                        : MTLVertexFormatUChar4Normalized;
    case xenos::VertexFormat::k_16_16:
      if (fetch.attributes.is_integer) {
        return fetch.attributes.is_signed ? MTLVertexFormatShort2
                                          : MTLVertexFormatUShort2;
      }
      return fetch.attributes.is_signed ? MTLVertexFormatShort2Normalized
                                        : MTLVertexFormatUShort2Normalized;
    case xenos::VertexFormat::k_16_16_16_16:
      if (fetch.attributes.is_integer) {
        return fetch.attributes.is_signed ? MTLVertexFormatShort4
                                          : MTLVertexFormatUShort4;
      }
      return fetch.attributes.is_signed ? MTLVertexFormatShort4Normalized
                                        : MTLVertexFormatUShort4Normalized;
    case xenos::VertexFormat::k_16_16_FLOAT:
      return MTLVertexFormatHalf2;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      return MTLVertexFormatHalf4;
    case xenos::VertexFormat::k_32:
      if (fetch.attributes.is_integer) {
        return fetch.attributes.is_signed ? MTLVertexFormatInt
                                          : MTLVertexFormatUInt;
      }
      return MTLVertexFormatFloat;
    case xenos::VertexFormat::k_32_32:
      if (fetch.attributes.is_integer) {
        return fetch.attributes.is_signed ? MTLVertexFormatInt2
                                          : MTLVertexFormatUInt2;
      }
      return MTLVertexFormatFloat2;
    case xenos::VertexFormat::k_32_32_32_32:
      if (fetch.attributes.is_integer) {
        return fetch.attributes.is_signed ? MTLVertexFormatInt4
                                          : MTLVertexFormatUInt4;
      }
      return MTLVertexFormatFloat4;
    case xenos::VertexFormat::k_32_FLOAT:
      return MTLVertexFormatFloat;
    case xenos::VertexFormat::k_32_32_FLOAT:
      return MTLVertexFormatFloat2;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return MTLVertexFormatFloat3;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      return MTLVertexFormatFloat4;
    // No direct, universally available MTL vertex format equivalent.
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    default:
      return MTLVertexFormatInvalid;
  }
}

static bool BuildVertexDescriptorFromShaderFetch(
    const MetalShader& shader, MTLVertexDescriptor* vd) {
  const auto& bindings = shader.vertex_bindings();
  if (bindings.empty()) {
    return false;
  }

  bool has_any_attribute = false;
  uint32_t reflection_attr_i = 0;
  uint32_t fallback_attr_i = 0;
  const auto& reflection = shader.reflection_info();

  for (const Shader::VertexBinding& binding : bindings) {
    if (binding.binding_index < 0 || binding.binding_index >= 31) {
      continue;
    }
    const uint32_t buffer_index = static_cast<uint32_t>(binding.binding_index);
    vd.layouts[buffer_index].stride = binding.stride_words * sizeof(uint32_t);
    vd.layouts[buffer_index].stepRate = 1;
    vd.layouts[buffer_index].stepFunction = MTLVertexStepFunctionPerVertex;

    for (const auto& attribute : binding.attributes) {
      uint32_t attribute_index = fallback_attr_i++;
      if (reflection_attr_i < reflection.vertex_inputs.size()) {
        attribute_index =
            static_cast<uint32_t>(reflection.vertex_inputs[reflection_attr_i]
                                      .attribute_index);
      }
      ++reflection_attr_i;

      if (attribute_index >= 31) {
        continue;
      }
      if (attribute.fetch_instr.attributes.offset < 0) {
        continue;
      }

      MTLVertexFormat metal_format = GetMetalVertexFormatForFetch(attribute);
      if (metal_format == MTLVertexFormatInvalid) {
        continue;
      }

      vd.attributes[attribute_index].format = metal_format;
      vd.attributes[attribute_index].offset =
          static_cast<uint32_t>(attribute.fetch_instr.attributes.offset);
      vd.attributes[attribute_index].bufferIndex = buffer_index;
      has_any_attribute = true;
    }
  }

  return has_any_attribute;
}

// ============================================================================
// RenderPipelineKey
// ============================================================================

bool MetalPipelineCache::RenderPipelineKey::operator==(
    const RenderPipelineKey& o) const {
  if (vertex_shader_hash != o.vertex_shader_hash ||
      pixel_shader_hash != o.pixel_shader_hash ||
      vertex_shader_modification != o.vertex_shader_modification ||
      pixel_shader_modification != o.pixel_shader_modification ||
      vertex_layout_hash != o.vertex_layout_hash ||
      depth_format != o.depth_format ||
      color_target_count != o.color_target_count ||
      sample_count != o.sample_count ||
      has_vertex_index != o.has_vertex_index) {
    return false;
  }
  for (uint32_t i = 0; i < color_target_count; ++i) {
    if (color_formats[i] != o.color_formats[i]) return false;
    const auto& a = blend_states[i];
    const auto& b = o.blend_states[i];
    if (a.blend_enable != b.blend_enable) return false;
    if (a.blend_enable) {
      if (a.src_blend != b.src_blend || a.dst_blend != b.dst_blend ||
          a.blend_op != b.blend_op ||
          a.src_blend_alpha != b.src_blend_alpha ||
          a.dst_blend_alpha != b.dst_blend_alpha ||
          a.blend_op_alpha != b.blend_op_alpha) {
        return false;
      }
    }
    if (a.write_mask != b.write_mask) return false;
  }
  return true;
}

size_t MetalPipelineCache::RenderPipelineKeyHash::operator()(
    const RenderPipelineKey& key) const {
  // FNV-1a style hash.
  size_t h = 14695981039346656037ULL;
  auto mix = [&](uint64_t val) {
    h ^= val;
    h *= 1099511628211ULL;
  };
  mix(key.vertex_shader_hash);
  mix(key.pixel_shader_hash);
  mix(key.vertex_shader_modification);
  mix(key.pixel_shader_modification);
  mix(key.vertex_layout_hash);
  mix(key.depth_format);
  mix(key.color_target_count);
  mix(key.sample_count);
  for (uint32_t i = 0; i < key.color_target_count; ++i) {
    mix(key.color_formats[i]);
    mix(key.blend_states[i].blend_enable ? 1 : 0);
    mix(key.blend_states[i].write_mask);
    if (key.blend_states[i].blend_enable) {
      mix(key.blend_states[i].src_blend);
      mix(key.blend_states[i].dst_blend);
      mix(key.blend_states[i].blend_op);
      mix(key.blend_states[i].src_blend_alpha);
      mix(key.blend_states[i].dst_blend_alpha);
      mix(key.blend_states[i].blend_op_alpha);
    }
  }
  return h;
}

// ============================================================================
// DepthStencilKey
// ============================================================================

bool MetalPipelineCache::DepthStencilKey::operator==(
    const DepthStencilKey& o) const {
  return depth_test_enable == o.depth_test_enable &&
         depth_write_enable == o.depth_write_enable &&
         depth_compare_func == o.depth_compare_func &&
         stencil_enable == o.stencil_enable &&
         stencil_read_mask == o.stencil_read_mask &&
         stencil_write_mask == o.stencil_write_mask &&
         stencil_front_compare == o.stencil_front_compare &&
         stencil_front_pass == o.stencil_front_pass &&
         stencil_front_fail == o.stencil_front_fail &&
         stencil_front_depth_fail == o.stencil_front_depth_fail &&
         stencil_back_compare == o.stencil_back_compare &&
         stencil_back_pass == o.stencil_back_pass &&
         stencil_back_fail == o.stencil_back_fail &&
         stencil_back_depth_fail == o.stencil_back_depth_fail;
}

size_t MetalPipelineCache::DepthStencilKeyHash::operator()(
    const DepthStencilKey& key) const {
  size_t h = 14695981039346656037ULL;
  auto mix = [&](uint64_t val) {
    h ^= val;
    h *= 1099511628211ULL;
  };
  mix(key.depth_test_enable);
  mix(key.depth_write_enable);
  mix(key.depth_compare_func);
  mix(key.stencil_enable);
  mix(key.stencil_read_mask);
  mix(key.stencil_write_mask);
  mix(key.stencil_front_compare);
  mix(key.stencil_front_pass);
  mix(key.stencil_front_fail);
  mix(key.stencil_front_depth_fail);
  mix(key.stencil_back_compare);
  mix(key.stencil_back_pass);
  mix(key.stencil_back_fail);
  mix(key.stencil_back_depth_fail);
  return h;
}

// ============================================================================
// MetalPipelineCache
// ============================================================================

MetalPipelineCache::MetalPipelineCache(
    MetalCommandProcessor& command_processor)
    : command_processor_(command_processor) {}

MetalPipelineCache::~MetalPipelineCache() {
  Shutdown();
}

bool MetalPipelineCache::Initialize() {
  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) return false;

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      provider.GetAdapterVendorID(),
      false,  // bindless_resources_used
      false,  // edram_rov_used
      false,  // gamma_render_target_as_srgb
      provider.caps().supports_msaa_2x,
      1,  // draw_resolution_scale_x
      1,  // draw_resolution_scale_y
      false);

  // Compile fallback shader library.
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kFallbackShaderSource];

  // We need function constants for the has_texture boolean.
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];

  fallback_library_ = [device newLibraryWithSource:source
                                           options:options
                                             error:&error];
  if (!fallback_library_) {
    REXGPU_ERROR("MetalPipelineCache: Failed to compile fallback shaders: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return false;
  }

  // Get the fallback functions.
  // For the fragment shader, create with function constants.
  fallback_vertex_func_ =
      [fallback_library_ newFunctionWithName:@"fallback_vertex"];

  // Create fragment function with has_texture = false for now.
  MTLFunctionConstantValues* constants =
      [[MTLFunctionConstantValues alloc] init];
  bool has_tex = false;
  [constants setConstantValue:&has_tex type:MTLDataTypeBool atIndex:0];

  fallback_fragment_func_ =
      [fallback_library_ newFunctionWithName:@"fallback_fragment"
                              constantValues:constants
                                       error:&error];
  if (!fallback_fragment_func_) {
    // Fallback to non-specialised version if function constants not supported.
    fallback_fragment_func_ =
        [fallback_library_ newFunctionWithName:@"fallback_fragment"];
  }

  resolve_compute_func_ =
      [fallback_library_ newFunctionWithName:@"resolve_edram_to_texture"];
  store_compute_func_ =
      [fallback_library_ newFunctionWithName:@"store_texture_to_edram"];

  if (!fallback_vertex_func_ || !fallback_fragment_func_) {
    REXGPU_ERROR("MetalPipelineCache: Failed to find fallback shader functions");
    return false;
  }

  if (resolve_compute_func_) {
    REXGPU_INFO("MetalPipelineCache: EDRAM resolve compute kernel available");
  }
  if (store_compute_func_) {
    REXGPU_INFO("MetalPipelineCache: EDRAM store compute kernel available");
  }

  REXGPU_INFO("MetalPipelineCache: Initialized with fallback shaders + "
         "resolve kernel");

  // Initialize the shader translation pipeline.
  // Both converters are optional — if either is unavailable, we fall back
  // to the magenta passthrough shaders (which is the current behavior).
  bool dxbc_translator_ok = shader_translator_ != nullptr;
  bool dxbc_ok = dxbc_to_dxil_converter_.Initialize();
  bool msc_ok = metal_shader_converter_.Initialize();
  shader_pipeline_available_ = dxbc_translator_ok && dxbc_ok && msc_ok;

  if (shader_pipeline_available_) {
    // Set minimum target for Apple Silicon.
    // NOTE: This is called after Initialize(), which eagerly creates the cached
    // root signature. That's fine — root signatures don't depend on the minimum
    // target. But SetMinimumTarget() MUST be called before any
    // ConvertWithStageEx() calls, which use the target for compilation.
    //
    // Fix 3: Auto-detect the running macOS version instead of hardcoding
    // "15.0". On macOS 26 (Tahoe) passing "15.0" causes MSC to generate
    // Metal IR without macOS-26-specific features and may trigger validation
    // failures because the version string is below the running OS version.
    {
      NSOperatingSystemVersion osVer =
          [[NSProcessInfo processInfo] operatingSystemVersion];
      // MSC expects "MAJOR.MINOR" format matching the OS marketing version.
      std::string os_version_str =
          fmt::format("{}.{}", (long)osVer.majorVersion,
                      (long)osVer.minorVersion);
      REXGPU_INFO("MetalPipelineCache: Setting MSC minimum target macOS {}",
                  os_version_str);
      metal_shader_converter_.SetMinimumTarget(
          /* IRGPUFamilyApple7 */ 1007,
          /* IROperatingSystem_macOS */ 0,
          os_version_str);
    }
    REXGPU_INFO("MetalPipelineCache: Shader translation pipeline READY "
           "(DXBC→DXIL→Metal IR)");
  } else {
    REXGPU_INFO("MetalPipelineCache: Shader translation pipeline NOT available "
           "(dxbc_translator={}, dxilconv={}, MSC={}). Using fallback shaders.",
           dxbc_translator_ok ? "ok" : "missing",
           dxbc_ok ? "ok" : "missing", msc_ok ? "ok" : "missing");
  }

  return true;
}

void MetalPipelineCache::Shutdown() {
  ClearCache();
  shader_cache_.Shutdown();
  shader_translator_.reset();
  shader_pipeline_available_ = false;
  fallback_vertex_func_ = nil;
  fallback_fragment_func_ = nil;
  resolve_compute_func_ = nil;
  store_compute_func_ = nil;
  fallback_library_ = nil;
}

void MetalPipelineCache::ClearCache() {
  render_pipeline_cache_.clear();
  depth_stencil_cache_.clear();
  active_shader_modifications_.clear();
}

void MetalPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id,
    bool blocking) {
  shader_storage_root_ = cache_root;
  current_title_id_ = title_id;

  // Initialize the shader disk cache for this title.
  if (shader_pipeline_available_) {
    // Include title_id in the cache path to prevent hypothetical
    // cross-title cache collisions (same ucode hash + modification + stage).
    char title_dir[32];
    std::snprintf(title_dir, sizeof(title_dir), "metal_shader_cache_%08X",
                  title_id);
    auto cache_path = cache_root / title_dir;
    shader_cache_.Initialize(cache_path);
  }
  REXGPU_INFO("MetalPipelineCache: Shader storage init for title {:08X}", title_id);
}

// ============================================================================
// Shader translation
// ============================================================================

bool MetalPipelineCache::TranslateShader(MetalShader* shader,
                                         uint64_t modification) {
  if (!shader) return false;
  if (shader->is_valid()) {
    auto active_it =
        active_shader_modifications_.find(shader->ucode_data_hash());
    if (active_it != active_shader_modifications_.end() &&
        active_it->second == modification) {
      return true;
    }
  }

  if (!shader_pipeline_available_ || !shader_translator_) {
    // Translation pipeline not available — use fallback shaders.
    shader->set_valid(false);
    return false;
  }

  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) {
    shader->set_valid(false);
    return false;
  }

  // Fast path: check the shader disk cache for a pre-compiled metallib.
  uint64_t cache_key = MetalShaderCache::GetCacheKey(
      shader->ucode_data_hash(), modification,
      static_cast<uint32_t>(shader->type()));

  if (shader_cache_.IsInitialized()) {
    MetalShaderCache::CachedMetallib cached;
    if (shader_cache_.Load(cache_key, &cached)) {
      // Create MTLLibrary directly from the cached metallib data.
      NSError* error = nil;
      dispatch_data_t data = dispatch_data_create(
          cached.metallib_data.data(), cached.metallib_data.size(),
          nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
#if !__has_feature(objc_arc)
      dispatch_release(data);
#endif

      if (library) {
        NSString* func_name =
            [NSString stringWithUTF8String:cached.function_name.c_str()];
        id<MTLFunction> function = [library newFunctionWithName:func_name];

        // Fallback: try alternative function names that MSC may generate.
        // Mirrors the fallback search in MetalShader::TranslateToMetal().
        if (!function) {
          NSArray* alt_names = @[ @"main0", @"main", @"vertexMain", @"fragmentMain" ];
          for (NSString* alt in alt_names) {
            function = [library newFunctionWithName:alt];
            if (function) {
              REXGPU_DEBUG("MetalPipelineCache: Cache hit used alt name '{}'",
                     [alt UTF8String]);
              break;
            }
          }
        }

        if (function) {
          shader->SetMetalLibrary(library, function);
          active_shader_modifications_[shader->ucode_data_hash()] =
              modification;
          REXGPU_DEBUG("MetalPipelineCache: Shader {:016X} loaded from cache",
                 shader->ucode_data_hash());
          return true;
        }
      }
      // Cache hit but failed to create library/function — fall through
      // to full translation.
    }
  }

  if (!shader->is_ucode_analyzed()) {
    shader->AnalyzeUcode(ucode_disasm_buffer_);
  }

  auto* translation = static_cast<DxbcShader::DxbcTranslation*>(
      shader->GetOrCreateTranslation(modification));
  if (!translation) {
    shader->set_valid(false);
    return false;
  }

  if (!translation->is_translated()) {
    if (!shader_translator_->TranslateAnalyzedShader(*translation)) {
      REXGPU_DEBUG(
          "MetalPipelineCache: Shader {:016X} DXBC translation failed "
          "(mod={:016X})",
          shader->ucode_data_hash(), modification);
      shader->set_valid(false);
      return false;
    }
  }
  if (!translation->is_valid()) {
    shader->set_valid(false);
    return false;
  }

  const auto& dxbc_data = translation->translated_binary();
  if (dxbc_data.empty()) {
    REXGPU_DEBUG("MetalPipelineCache: Shader {:016X} has empty DXBC translation",
           shader->ucode_data_hash());
    shader->set_valid(false);
    return false;
  }

  // Dump DXBC for debugging if environment variable is set
  const char* dxbc_dump_dir = std::getenv("REX_DXBC_OUTPUT_DIR");
  if (dxbc_dump_dir) {
    char dump_path[512];
    std::snprintf(dump_path, sizeof(dump_path), 
                  "%s/shader_%016llX_mod_%016llX.dxbc",
                  dxbc_dump_dir, 
                  (unsigned long long)shader->ucode_data_hash(),
                  (unsigned long long)modification);
    FILE* f = std::fopen(dump_path, "wb");
    if (f) {
      std::fwrite(dxbc_data.data(), 1, dxbc_data.size(), f);
      std::fclose(f);
      REXGPU_DEBUG("MetalPipelineCache: Dumped DXBC to {}", dump_path);
    }
  }

  // Run the full pipeline: DXBC → DXIL → Metal IR → MTLLibrary.
  bool success = shader->TranslateToMetal(
      device, dxbc_data, dxbc_to_dxil_converter_, metal_shader_converter_);

  if (success) {
    active_shader_modifications_[shader->ucode_data_hash()] = modification;
    REXGPU_DEBUG(
        "MetalPipelineCache: Shader {:016X} translated successfully "
        "(mod={:016X})",
        shader->ucode_data_hash(), modification);
    // Store the compiled metallib to disk cache.
    if (shader_cache_.IsInitialized()) {
      shader_cache_.Store(cache_key, shader->metal_function_name(),
                          shader->metallib_data().data(),
                          shader->metallib_data().size());
    }
    // Release intermediate DXIL and metallib bytes — the MTLLibrary/MTLFunction
    // are now live, and the data has been persisted to the disk cache.
    shader->ClearIntermediateData();
  } else {
    REXGPU_ERROR(
        "MetalPipelineCache: Shader {:016X} translation failed — "
        "using fallback (mod={:016X}). DXBC size: {} bytes. "
        "Check /tmp/rex_shader_dumps/ for dumped shader if REX_DXBC_OUTPUT_DIR is set.",
        shader->ucode_data_hash(), modification, dxbc_data.size());
    shader->set_valid(false);
  }

  return success;
}

// ============================================================================
// Render pipeline state creation
// ============================================================================

id<MTLRenderPipelineState> MetalPipelineCache::GetOrCreateRenderPipelineState(
    const RenderPipelineKey& key,
    MetalShader* vertex_shader,
    MetalShader* pixel_shader) {
  // Check the cache first.
  auto it = render_pipeline_cache_.find(key);
  if (it != render_pipeline_cache_.end()) {
    return it->second;
  }

  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) return nil;

  // Select vertex/fragment functions.
  id<MTLFunction> vs_func = nil;
  id<MTLFunction> fs_func = nil;

  // CRITICAL: Always validate fallback functions are available
  if (!fallback_vertex_func_ || !fallback_fragment_func_) {
    REXGPU_ERROR("MetalPipelineCache: Fallback shaders not initialized");
    return nil;
  }

  if (vertex_shader && vertex_shader->is_valid() &&
      vertex_shader->function()) {
    vs_func = vertex_shader->function();
  } else {
    vs_func = fallback_vertex_func_;
  }

  if (pixel_shader && pixel_shader->is_valid() &&
      pixel_shader->function()) {
    fs_func = pixel_shader->function();
  } else {
    fs_func = fallback_fragment_func_;
  }

  // Double-check functions are valid before proceeding
  if (!vs_func || !fs_func) {
    REXGPU_ERROR("MetalPipelineCache: No vertex or fragment function available (vs={}, fs={})",
                 vs_func ? "ok" : "nil", fs_func ? "ok" : "nil");
    return nil;
  }
  
  REXGPU_DEBUG("MetalPipelineCache: Creating PSO with vs={} fs={}", 
               vertex_shader && vertex_shader->is_valid() ? "translated" : "fallback",
               pixel_shader && pixel_shader->is_valid() ? "translated" : "fallback");

  // Build the pipeline descriptor.
  MTLRenderPipelineDescriptor* desc =
      [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vs_func;
  desc.fragmentFunction = fs_func;

  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];

  bool used_fetch_layout = false;
  if (REXCVAR_GET(metal_vertex_layout_from_fetch) &&
      vertex_shader && vertex_shader->is_valid()) {
    used_fetch_layout = BuildVertexDescriptorFromShaderFetch(*vertex_shader, vd);
  }

  if (!used_fetch_layout) {
    // Vertex descriptor for the fallback shader.
    // Matches FallbackVertexIn: position(float4), color(float4), texcoord(float2)
    vd.attributes[0].format = MTLVertexFormatFloat4;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 16;
    vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2;
    vd.attributes[2].offset = 32;
    vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride = 40;
    vd.layouts[0].stepRate = 1;
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
  }

  desc.vertexDescriptor = vd;

  // Color attachments.
  for (uint32_t i = 0; i < key.color_target_count && i < 4; ++i) {
    auto ca = desc.colorAttachments[i];
    ca.pixelFormat = (MTLPixelFormat)key.color_formats[i];

    const auto& bs = key.blend_states[i];
    ca.writeMask = MTLColorWriteMaskNone;
    if (bs.write_mask & 0x1) ca.writeMask |= MTLColorWriteMaskRed;
    if (bs.write_mask & 0x2) ca.writeMask |= MTLColorWriteMaskGreen;
    if (bs.write_mask & 0x4) ca.writeMask |= MTLColorWriteMaskBlue;
    if (bs.write_mask & 0x8) ca.writeMask |= MTLColorWriteMaskAlpha;

    if (bs.blend_enable) {
      ca.blendingEnabled = YES;
      ca.rgbBlendOperation = (MTLBlendOperation)bs.blend_op;
      ca.alphaBlendOperation = (MTLBlendOperation)bs.blend_op_alpha;
      ca.sourceRGBBlendFactor = (MTLBlendFactor)bs.src_blend;
      ca.destinationRGBBlendFactor = (MTLBlendFactor)bs.dst_blend;
      ca.sourceAlphaBlendFactor = (MTLBlendFactor)bs.src_blend_alpha;
      ca.destinationAlphaBlendFactor = (MTLBlendFactor)bs.dst_blend_alpha;
    } else {
      ca.blendingEnabled = NO;
    }
  }

  // Depth attachment.
  if (key.depth_format != 0) {
    desc.depthAttachmentPixelFormat = (MTLPixelFormat)key.depth_format;
    // Most Xenos depth formats include stencil.
    if (key.depth_format == (uint32_t)MTLPixelFormatDepth32Float_Stencil8) {
      desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
  }

  // MSAA.
  desc.rasterSampleCount = key.sample_count;

  desc.label = [NSString stringWithFormat:@"PSO_vs%llx_ps%llx",
                key.vertex_shader_hash, key.pixel_shader_hash];

  // Create the pipeline state.
  NSError* error = nil;
  id<MTLRenderPipelineState> pso =
      [device newRenderPipelineStateWithDescriptor:desc error:&error];
  if (!pso) {
    REXGPU_ERROR("MetalPipelineCache: Failed to create render PSO: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return nil;
  }

  render_pipeline_cache_[key] = pso;
  REXGPU_DEBUG("MetalPipelineCache: Created render PSO (cache size: {})",
            render_pipeline_cache_.size());
  return pso;
}

// ============================================================================
// Depth/stencil state creation
// ============================================================================

id<MTLDepthStencilState> MetalPipelineCache::GetOrCreateDepthStencilState(
    const DepthStencilKey& key) {
  auto it = depth_stencil_cache_.find(key);
  if (it != depth_stencil_cache_.end()) {
    return it->second;
  }

  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) return nil;

  MTLDepthStencilDescriptor* desc =
      [[MTLDepthStencilDescriptor alloc] init];

  if (key.depth_test_enable) {
    desc.depthCompareFunction = (MTLCompareFunction)key.depth_compare_func;
    desc.depthWriteEnabled = key.depth_write_enable ? YES : NO;
  } else {
    desc.depthCompareFunction = MTLCompareFunctionAlways;
    desc.depthWriteEnabled = NO;
  }

  if (key.stencil_enable) {
    MTLStencilDescriptor* front = [[MTLStencilDescriptor alloc] init];
    front.stencilCompareFunction =
        (MTLCompareFunction)key.stencil_front_compare;
    front.stencilFailureOperation =
        (MTLStencilOperation)key.stencil_front_fail;
    front.depthFailureOperation =
        (MTLStencilOperation)key.stencil_front_depth_fail;
    front.depthStencilPassOperation =
        (MTLStencilOperation)key.stencil_front_pass;
    front.readMask = key.stencil_read_mask;
    front.writeMask = key.stencil_write_mask;
    desc.frontFaceStencil = front;

    MTLStencilDescriptor* back = [[MTLStencilDescriptor alloc] init];
    back.stencilCompareFunction =
        (MTLCompareFunction)key.stencil_back_compare;
    back.stencilFailureOperation =
        (MTLStencilOperation)key.stencil_back_fail;
    back.depthFailureOperation =
        (MTLStencilOperation)key.stencil_back_depth_fail;
    back.depthStencilPassOperation =
        (MTLStencilOperation)key.stencil_back_pass;
    back.readMask = key.stencil_read_mask;
    back.writeMask = key.stencil_write_mask;
    desc.backFaceStencil = back;
  }

  id<MTLDepthStencilState> state =
      [device newDepthStencilStateWithDescriptor:desc];
  if (!state) {
    REXGPU_ERROR("MetalPipelineCache: Failed to create depth/stencil state");
    return nil;
  }

  depth_stencil_cache_[key] = state;
  return state;
}

}  // namespace rex::graphics::metal
