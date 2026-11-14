/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/hash.h>
#include <rex/assert.h>
#include <rex/types.h>
#include "gpu/xenos/d3d12/shader.h"
#include "gpu/xenos/d3d12/shared_memory.h"
#include "gpu/xenos/pipeline/texture/cache.h"
#include <rex/utility.h>
#include <rex/xenos/texture.h>
#include <rex/xenos/register.h>
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class D3D12TextureCache final : public TextureCache {
  struct Key {};

 public:
  explicit D3D12TextureCache(Key, const xenos::RegisterFile& register_file,
                             const XenosSettings& settings, D3D12SharedMemory& shared_memory,
                             u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                             D3D12CommandProcessor& command_processor,
                             bool bindless_resources_used);

  struct TextureSRVKey {
    TextureKey key;
    u32 host_swizzle;
    u8 swizzled_signs;
  };

  union SamplerParameters {
    u32 value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      xenos::BorderColor border_color : 2;
      /** All three are true for anisotropic filtering. */
      u32 mag_linear : 1;
      u32 min_linear : 1;
      u32 mip_linear : 1;
      xenos::AnisoFilter aniso_filter : 3;
      /** mip_base_map limits fetching to mip_min_level, the maximum level is in the texture. */
      u32 mip_min_level : 4;
      u32 mip_base_map : 1;
    };

    SamplerParameters() : value(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  static std::unique_ptr<D3D12TextureCache> Create(
      const xenos::RegisterFile& register_file, const XenosSettings& settings,
      D3D12SharedMemory& shared_memory, u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
      D3D12CommandProcessor& command_processor, bool bindless_resources_used) {
    auto texture_cache = std::make_unique<D3D12TextureCache>(
        Key{}, register_file, settings, shared_memory, draw_resolution_scale_x,
        draw_resolution_scale_y, command_processor, bindless_resources_used);
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return std::move(texture_cache);
  }

  ~D3D12TextureCache();

  void ClearCache() override;

  void BeginSubmission(u64 new_submission_index) override;
  void BeginFrame() override;
  void EndFrame();

  /**
   * Creates and untiles the textures shaders need. May bind compute pipelines, so it must run
   * before the drawing pipeline is bound.
   *
   * @param used_texture_mask Mask of the used texture fetch constants.
   */
  void RequestTextures(u32 used_texture_mask) override;

  /**
   * Reports whether stored texture SRV keys match the current bindings and host shader binding
   * layout.
   *
   * @param host_shader_binding_count Element count of both arrays.
   */
  bool IsActiveTextureSRVKeysUpToDate(const TextureSRVKey* keys,
                                      const D3D12Shader::TextureBinding* host_shader_bindings,
                                      size_t host_shader_binding_count) const;
  /**
   * Writes the current bindings to texture SRV keys.
   *
   * @param keys Receives host_shader_binding_count keys.
   * @param host_shader_binding_count Element count of both arrays.
   */
  void WriteActiveTextureSRVKeys(TextureSRVKey* keys,
                                 const D3D12Shader::TextureBinding* host_shader_bindings,
                                 size_t host_shader_binding_count) const;
  void WriteActiveTextureBindfulSRV(const D3D12Shader::TextureBinding& host_shader_binding,
                                    D3D12_CPU_DESCRIPTOR_HANDLE handle);
  u32 GetActiveTextureBindlessSRVIndex(const D3D12Shader::TextureBinding& host_shader_binding);

  SamplerParameters GetSamplerParameters(const D3D12Shader::SamplerBinding& binding) const;
  /**
   * Writes a sampler descriptor. Direct3D 11.3 leaves undefined whether LOD clamping happens
   * before or after deciding if magnification occurs.
   *
   * @param handle Destination descriptor.
   */
  void WriteSampler(SamplerParameters parameters, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  /**
   * Reports whether the clamped scale is not smaller than the requested one.
   *
   * @param scale_x Horizontal scale, clamped in place.
   * @param scale_y Vertical scale, clamped in place.
   * @return True when the scale was not reduced.
   */
  static bool ClampDrawResolutionScaleToMaxSupported(u32& scale_x, u32& scale_y,
                                                     const gpu::d3d12::D3D12Provider& provider);
  /**
   * Allocates the tiles backing the range.
   *
   * @param start_unscaled Start of the range in unscaled bytes.
   * @param length_unscaled Length of the range in unscaled bytes.
   */
  bool CommitScaledResolveMemory(u32 start_unscaled, u32 length_unscaled,
                                 u32 length_scaled_alignment_log2 = 0) override;
  /**
   * Makes the range of up to 1 to 2 GB accessible on the GPU. A draw call can access only one
   * range.
   *
   * @param start_unscaled Start of the range in unscaled bytes.
   * @param length_unscaled Length of the range in unscaled bytes.
   * @return True when the range is current.
   */
  bool UpdateScaledResolveRange(u32 start_unscaled, u32 length_unscaled,
                                u32 length_scaled_alignment_log2 = 0);
  /**
   * Creates a view of the range from the last successful UpdateScaledResolveRange call.
   *
   * @param handle Destination descriptor.
   * @param element_size_bytes_pow2 Log2 of the element size in bytes.
   */
  void CreateCurrentScaledResolveRangeUintPow2SRV(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                  u32 element_size_bytes_pow2);
  /**
   * Creates a UAV of the range from the last successful UpdateScaledResolveRange call.
   *
   * @param handle Destination descriptor.
   * @param element_size_bytes_pow2 Log2 of the element size in bytes.
   */
  void CreateCurrentScaledResolveRangeUintPow2UAV(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                  u32 element_size_bytes_pow2);
  void TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATES new_state);
  u64 GetCurrentScaledResolveRangeStart() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  u64 GetCurrentScaledResolveRangeLength() const {
    return scaled_resolve_current_range_length_scaled_;
  }
  ID3D12Resource* GetCurrentScaledResolveBufferResource() {
    return GetCurrentScaledResolveBuffer().GetResource();
  }
  /**
   * Returns the index of the current scaled resolve buffer, which is also its offset in
   * gigabytes.
   */
  size_t GetCurrentScaledResolveBufferIndex() const {
    return scaled_resolve_1gb_buffer_indices_[scaled_resolve_current_range_start_scaled_ >> 30];
  }
  void MarkCurrentScaledResolveRangeUAVWritesCommitNeeded() {
    REX_ASSERT(IsDrawResolutionScaled());
    GetCurrentScaledResolveBuffer().SetUAVBarrierPending();
  }

  /**
   * Returns the front buffer texture in NON_PIXEL_SHADER_RESOURCE state and writes its SRV
   * description, or null on failure. May call LoadTextureData, so the caller must not change
   * descriptor heaps around it.
   *
   * @param srv_desc_out Receives the SRV description.
   * @param format_out Receives the texture format.
   * @param width_unscaled_out Receives the unscaled width, may be null.
   * @param height_unscaled_out Receives the unscaled height, may be null.
   */
  ID3D12Resource* RequestSwapTexture(D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out,
                                     xenos::TextureFormat& format_out,
                                     u32* width_unscaled_out = nullptr,
                                     u32* height_unscaled_out = nullptr);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  u32 GetHostFormatSwizzle(TextureKey key) const override;

  u32 GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  u32 GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  /** Binds pipelines, allocates descriptors and copies the texture data. */
  bool LoadHostTextureDataFromResidentMemory(Texture& texture, bool load_base,
                                             bool load_mips) override;

  void UpdateHostTextureBindings(u32 fetch_constant_mask) override;

 private:
  struct HostFormat {
    /** Typeless when the signedness or number representation differs. */
    DXGI_FORMAT dxgi_format_resource;
    DXGI_FORMAT dxgi_format_unsigned;
    /** Used when the signed and decompressing shaders are not needed. */
    LoadShaderIndex load_shader;
    DXGI_FORMAT dxgi_format_signed;
    /**
     * Load shader for the signed version when it needs a different host bit representation.
     * Otherwise load_shader is used.
     */
    LoadShaderIndex load_shader_signed;

    // No integer DXGI formats here, as they are not filterable and only readable with Load.

    /** True when the DXGI format consists of blocks, so copy regions must align to them. */
    bool is_block_compressed;
    /**
     * Used for unaligned sizes, as Direct3D on PC cannot create such block compressed textures.
     * Supports unsigned normalized formats only.
     */
    DXGI_FORMAT dxgi_format_uncompressed;
    LoadShaderIndex load_shader_decompress;

    u32 swizzle;
  };

  class D3D12Texture final : public Texture {
   public:
    union SRVDescriptorKey {
      u32 key;
      struct {
        u32 is_signed : 1;
        u32 host_swizzle : 12;
        u32 dimension : 2;
      };

      SRVDescriptorKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

      using Hasher = rex::KeyHasher<SRVDescriptorKey>;
      bool operator==(const SRVDescriptorKey& other_key) const { return key == other_key.key; }
      bool operator!=(const SRVDescriptorKey& other_key) const { return !(*this == other_key); }
    };

    ID3D12Resource* GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATES end_state);

    explicit D3D12Texture(D3D12TextureCache& texture_cache, const TextureKey& key,
                          ID3D12Resource* resource, D3D12_RESOURCE_STATES resource_state,
                          bool track_usage = true);
    ~D3D12Texture();

    ID3D12Resource* GetResource() const { return resource_.Get(); }

    D3D12_RESOURCE_STATES SetResourceState(D3D12_RESOURCE_STATES new_state) {
      D3D12_RESOURCE_STATES old_state = resource_state_;
      resource_state_ = new_state;
      return old_state;
    }

    u32 GetSRVDescriptorIndex(SRVDescriptorKey descriptor_key) const {
      auto it = srv_descriptors_.find(descriptor_key);
      return it != srv_descriptors_.cend() ? it->second : UINT32_MAX;
    }

    void AddSRVDescriptorIndex(SRVDescriptorKey descriptor_key, u32 descriptor_index) {
      srv_descriptors_.emplace(descriptor_key, descriptor_index);
    }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12_RESOURCE_STATES resource_state_;
    std::unique_ptr<D3D12Texture> texture_3d_as_2d_;

    /** Bindful uses the non shader visible cache, bindless the global shader visible heap. */
    std::unordered_map<SRVDescriptorKey, u32, SRVDescriptorKey::Hasher> srv_descriptors_;
  };

  static constexpr u32 kSRVDescriptorCachePageSize = 65536;

  struct SRVDescriptorCachePage : public rex::MoveOnly {
   public:
    explicit SRVDescriptorCachePage(ID3D12DescriptorHeap* heap)
        : heap_(heap), heap_start_(heap->GetCPUDescriptorHandleForHeapStart()) {}
    SRVDescriptorCachePage(SRVDescriptorCachePage&& page) = default;
    SRVDescriptorCachePage& operator=(SRVDescriptorCachePage&& page) = default;

    ID3D12DescriptorHeap* GetHeap() const { return heap_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetHeapStart() const { return heap_start_; }

   private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start_;
  };

  struct D3D12TextureBinding {
    u32 descriptor_index;
    u32 descriptor_index_signed;

    D3D12TextureBinding() { Reset(); }

    void Reset() {
      descriptor_index = UINT32_MAX;
      descriptor_index_signed = UINT32_MAX;
    }
  };

  class ScaledResolveVirtualBuffer {
   public:
    explicit ScaledResolveVirtualBuffer(ID3D12Resource* resource,
                                        D3D12_RESOURCE_STATES resource_state)
        : resource_(resource), resource_state_(resource_state) {}
    ID3D12Resource* GetResource() const { return resource_.Get(); }
    D3D12_RESOURCE_STATES SetResourceState(D3D12_RESOURCE_STATES new_state) {
      D3D12_RESOURCE_STATES old_state = resource_state_;
      if (old_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        uav_barrier_pending_ = false;
      }
      resource_state_ = new_state;
      return old_state;
    }
    void SetUAVBarrierPending() {
      if (resource_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        uav_barrier_pending_ = true;
      }
    }
    void ClearUAVBarrierPending() { uav_barrier_pending_ = false; }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12_RESOURCE_STATES resource_state_;
    bool uav_barrier_pending_ = false;
  };

  bool Initialize();

  /** Direct3D on PC only creates block compressed textures with 4x4 aligned dimensions. */
  bool IsDecompressionNeeded(xenos::TextureFormat format, u32 width, u32 height) const;
  DXGI_FORMAT GetDXGIResourceFormat(xenos::TextureFormat format, u32 width, u32 height) const {
    const HostFormat& host_format = kHostFormats[static_cast<u32>(format)];
    return IsDecompressionNeeded(format, width, height) ? host_format.dxgi_format_uncompressed
                                                        : host_format.dxgi_format_resource;
  }
  DXGI_FORMAT GetDXGIResourceFormat(TextureKey key) const {
    return GetDXGIResourceFormat(key.format, key.GetWidth(), key.GetHeight());
  }
  DXGI_FORMAT GetDXGIUnormFormat(xenos::TextureFormat format, u32 width, u32 height) const {
    const HostFormat& host_format = kHostFormats[static_cast<u32>(format)];
    return IsDecompressionNeeded(format, width, height) ? host_format.dxgi_format_uncompressed
                                                        : host_format.dxgi_format_unsigned;
  }
  DXGI_FORMAT GetDXGIUnormFormat(TextureKey key) const {
    return GetDXGIUnormFormat(key.format, key.GetWidth(), key.GetHeight());
  }

  LoadShaderIndex GetLoadShaderIndex(TextureKey key) const;

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  /**
   * Returns the index of an existing or new descriptor, cached for bindful or global for bindless,
   * or UINT32_MAX if creation failed.
   *
   * @param is_signed True for the signed version.
   */
  u32 FindOrCreateTextureDescriptor(D3D12Texture& texture, xenos::DataDimension dimension,
                                    bool is_signed, u32 host_swizzle);
  void ReleaseTextureDescriptor(u32 descriptor_index) noexcept;
  D3D12_CPU_DESCRIPTOR_HANDLE GetTextureDescriptorCPUHandle(u32 descriptor_index) const;

  /**
   * Any range up to 1 GB is accessible through one or two buffers, and buffer N starts at N GB.
   * With 2x2 scale one buffer covers 2 GB, with 3x3 scale four buffers cover 4.5 GB.
   */
  size_t GetScaledResolveBufferCount() const {
    REX_ASSERT(IsDrawResolutionScaled());
    u64 address_space_size = static_cast<u64>(SharedMemory::kBufferSize) *
                             (u64{GetDrawResolutionScaleX()} * GetDrawResolutionScaleY());
    return static_cast<size_t>((address_space_size - 1) >> 30);
  }
  /**
   * Returns the indices of the two scaled resolve buffers that may cover the location. They are
   * the same near the ends of the address space.
   *
   * @param address_scaled Scaled address of the location.
   */
  std::array<size_t, 2> GetPossibleScaledResolveBufferIndices(u64 address_scaled) const {
    REX_ASSERT(IsDrawResolutionScaled());
    size_t address_gb = static_cast<size_t>(address_scaled >> 30);
    size_t max_index = GetScaledResolveBufferCount() - 1;
    return std::array<size_t, 2>{
        std::min(address_gb, max_index),
        std::min(std::max(address_gb, static_cast<size_t>(1)) - static_cast<size_t>(1), max_index)};
  }
  ScaledResolveVirtualBuffer& GetCurrentScaledResolveBuffer() {
    ScaledResolveVirtualBuffer* scaled_resolve_buffer =
        scaled_resolve_2gb_buffers_[GetCurrentScaledResolveBufferIndex()].get();
    REX_ASSERT(scaled_resolve_buffer != nullptr);
    return *scaled_resolve_buffer;
  }

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  static const HostFormat kHostFormats[64];

  D3D12CommandProcessor& command_processor_;
  bool bindless_resources_used_;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> load_root_signature_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kLoadShaderCount> load_pipelines_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kLoadShaderCount> load_pipelines_scaled_;

  std::vector<SRVDescriptorCachePage> srv_descriptor_cache_;
  u32 srv_descriptor_cache_allocated_;
  std::vector<u32> srv_descriptor_cache_free_;
  u32 srv_descriptor_cache_free_count_ = 0;

  enum class NullSRVDescriptorIndex : u8 {
    k2DArray,
    k3D,
    kCube,

    kCount,
  };
  /** Null SRV descriptors by NullSRVDescriptorIndex, not shader visible. */
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> null_srv_descriptor_heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE null_srv_descriptor_heap_start_;

  std::array<D3D12TextureBinding, xenos::kTextureFetchConstantCount> d3d12_texture_bindings_;

  enum : u8 {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  u8 unsupported_format_features_used_[64];

  /**
   * Tiled buffers for resolved data with resolution scaling. The array is sized for
   * kMaxDrawResolutionScaleAlongAxis, so it matches GetScaledResolveBufferCount only at the
   * maximum scale.
   */
  std::array<std::unique_ptr<ScaledResolveVirtualBuffer>,
             (static_cast<u64>(SharedMemory::kBufferSize) *
                  (u64{kMaxDrawResolutionScaleAlongAxis} * kMaxDrawResolutionScaleAlongAxis) -
              1) /
                 (UINT32_C(1) << 30)>
      scaled_resolve_2gb_buffers_;
  /** 16 MB heaps, as they are needed sparsely. */
  static constexpr u32 kScaledResolveHeapSizeLog2 = 24;
  static constexpr u32 kScaledResolveHeapSize = static_cast<u32>(1) << kScaledResolveHeapSizeLog2;
  static_assert((kScaledResolveHeapSize % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) == 0,
                "Scaled resolve heap size must be a multiple of Direct3D tile size");
  static_assert(kScaledResolveHeapSizeLog2 <= SharedMemory::kBufferSizeLog2,
                "Scaled resolve heaps are assumed to be wholly mappable irrespective of "
                "resolution scale, never truncated, for example, if the scaled resolve "
                "address space is 4.5 GB, but the heap size is 1 GB");
  static_assert(kScaledResolveHeapSizeLog2 <= 30,
                "Scaled resolve heaps are assumed to only be wholly mappable to up to "
                "2 buffers of 2 GB");
  std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> scaled_resolve_heaps_;
  u32 scaled_resolve_heap_count_ = 0;
  /** Last owning buffer index for each 1 GB. */
  size_t scaled_resolve_1gb_buffer_indices_[(static_cast<u64>(SharedMemory::kBufferSize) *
                                                 kMaxDrawResolutionScaleAlongAxis *
                                                 kMaxDrawResolutionScaleAlongAxis +
                                             ((static_cast<u32>(1) << 30) - 1)) >>
                                            30];
  u64 scaled_resolve_current_range_start_scaled_;
  u64 scaled_resolve_current_range_length_scaled_;
};

}  // namespace rex::gpu::d3d12
