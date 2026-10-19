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
#include <rex/xenos/texture_layout.h>
#include <rex/xenos/register_file.h>
#include <rex/xenos/xenos.h>
#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class D3D12TextureCache final : public TextureCache {
  struct Key {};

 public:
  explicit D3D12TextureCache(Key, const xenos::RegisterFile& register_file,
                             D3D12SharedMemory& shared_memory, u32 draw_resolution_scale_x,
                             u32 draw_resolution_scale_y, D3D12CommandProcessor& command_processor,
                             bool bindless_resources_used);

  /** Keys stored for checking whether descriptors for host shader bindings are up to date. */
  struct TextureSRVKey {
    TextureKey key;
    u32 host_swizzle;
    u8 swizzled_signs;
  };

  /** Sampler parameters convertible to a host sampler or usable to check sampler validity. */
  union SamplerParameters {
    u32 value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      xenos::BorderColor border_color : 2;
      /** All 3 are true for anisotropic filtering. */
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

  static std::unique_ptr<D3D12TextureCache> Create(const xenos::RegisterFile& register_file,
                                                   D3D12SharedMemory& shared_memory,
                                                   u32 draw_resolution_scale_x,
                                                   u32 draw_resolution_scale_y,
                                                   D3D12CommandProcessor& command_processor,
                                                   bool bindless_resources_used) {
    auto texture_cache = std::make_unique<D3D12TextureCache>(
        Key{}, register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y,
        command_processor, bindless_resources_used);
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
   * Creates and untiles the textures needed by shaders and puts them in the SRV state. Requires
   * a submission. May bind compute pipelines, notifying the command processor, so it must run
   * before the drawing pipeline is bound.
   *
   * @param used_texture_mask Mask of the used texture fetch constants.
   */
  void RequestTextures(u32 used_texture_mask) override;

  /**
   * Reports whether texture SRV keys stored externally are valid for the current bindings
   * and host shader binding layout. Both arrays must have host_shader_binding_count elements,
   * otherwise they are incompatible.
   *
   * @param keys Stored keys.
   * @param host_shader_bindings Host shader texture bindings.
   * @param host_shader_binding_count Element count of both arrays.
   * @return True when the keys are up to date.
   */
  bool AreActiveTextureSRVKeysUpToDate(const TextureSRVKey* keys,
                                       const D3D12Shader::TextureBinding* host_shader_bindings,
                                       size_t host_shader_binding_count) const;
  /**
   * Exports the current binding data to texture SRV keys, for checking whether subsequent draw
   * calls can keep using the same bindings.
   *
   * @param keys Receives host_shader_binding_count keys.
   * @param host_shader_bindings Host shader texture bindings.
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
   * Writes a sampler descriptor.
   *
   * Direct3D 11.3 leaves undefined whether LOD clamping with MinLOD and MaxLOD happens before or
   * after deciding if magnification occurs. For mip_base_map the GL_NEAREST and GL_LINEAR
   * minification emulation from the Vulkan VkSamplerCreateInfo specification is used, keeping
   * magnification versus minification, as point mip sampling (without anisotropic filtering)
   * with MaxLOD MinLOD + 0.25. With anisotropic filtering the filter is linear for both on
   * Direct3D 12, so magnification does not matter, but linear mip filtering must be avoided for
   * kBaseMap, so MaxLOD equals MinLOD.
   *
   * @param parameters Sampler parameters.
   * @param handle Destination descriptor.
   */
  void WriteSampler(SamplerParameters parameters, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  /**
   * Reports whether the clamped scale is not smaller than the requested one.
   *
   * @param scale_x Horizontal scale, clamped in place.
   * @param scale_y Vertical scale, clamped in place.
   * @param provider D3D12 provider.
   * @return True when the scale was not reduced.
   */
  static bool ClampDrawResolutionScaleToMaxSupported(u32& scale_x, u32& scale_y,
                                                     const gpu::d3d12::D3D12Provider& provider);
  /**
   * Allocates the tiles backing the range in the buffers.
   *
   * @param start_unscaled Start of the range in unscaled bytes.
   * @param length_unscaled Length of the range in unscaled bytes.
   * @param length_scaled_alignment_log2 Log2 of the scaled length alignment.
   * @return True when the tiles are allocated.
   */
  bool EnsureScaledResolveMemoryCommitted(u32 start_unscaled, u32 length_unscaled,
                                          u32 length_scaled_alignment_log2 = 0) override;
  /**
   * Makes the range of up to 1 to 2 GB accessible on the GPU. A draw call can access only 1
   * range, and the same memory is accessible through different buffers depending on the range,
   * so aliasing barriers are required.
   *
   * @param start_unscaled Start of the range in unscaled bytes.
   * @param length_unscaled Length of the range in unscaled bytes.
   * @param length_scaled_alignment_log2 Log2 of the scaled length alignment.
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
  u64 GetCurrentScaledResolveRangeStartScaled() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  u64 GetCurrentScaledResolveRangeLengthScaled() const {
    return scaled_resolve_current_range_length_scaled_;
  }
  ID3D12Resource* GetCurrentScaledResolveBufferResource() {
    return GetCurrentScaledResolveBuffer().GetResource();
  }
  size_t GetCurrentScaledResolveBufferIndexPublic() const {
    return GetCurrentScaledResolveBufferIndex();
  }
  void MarkCurrentScaledResolveRangeUAVWritesCommitNeeded() {
    REX_ASSERT(IsDrawResolutionScaled());
    GetCurrentScaledResolveBuffer().SetUAVBarrierPending();
  }

  /**
   * Returns the front buffer texture in NON_PIXEL_SHADER_RESOURCE state and writes its SRV
   * description. May call LoadTextureData, so the same restrictions on descriptor heap changes
   * apply.
   *
   * @param srv_desc_out Receives the SRV description.
   * @param format_out Receives the texture format.
   * @param width_unscaled_out Receives the unscaled width, may be null.
   * @param height_unscaled_out Receives the unscaled height, may be null.
   * @return The resource, or null on failure.
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

  /**
   * Binds pipelines, allocates descriptors and copies the texture data.
   *
   * Footprints are computed manually instead of with GetCopyableFootprints, as shaders may copy
   * multiple blocks along X per thread, so all rows including the last must be padded to the
   * blocks copied per invocation without assuming D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, and
   * GetCopyableFootprints aligns row offsets but not the total size. Scaled base and mips may use
   * different buffers, and addressing over 128 * 2^20 texels
   * (2^D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP) is not mandatory, so they need separate
   * source descriptors.
   *
   * @param texture Texture to load.
   * @param load_base True to load the base level.
   * @param load_mips True to load the mip levels.
   * @return True when the data was loaded.
   */
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

  void UpdateTextureBindingsImpl(u32 fetch_constant_mask) override;

 private:
  struct HostFormat {
    /** Typeless when the signedness or number representation differs. */
    DXGI_FORMAT dxgi_format_resource;
    /** SRV format for unsigned normalized or float. */
    DXGI_FORMAT dxgi_format_unsigned;
    /** Regular load shader, used when special ones (signed or decompressing) are not needed. */
    LoadShaderIndex load_shader;
    /** SRV format for signed normalized or float. */
    DXGI_FORMAT dxgi_format_signed;
    /**
     * Load shader for the signed version when it needs a different host bit representation.
     * Otherwise load_shader is used and a single copy is created if both versions are used.
     */
    LoadShaderIndex load_shader_signed;

    // No integer DXGI formats here, as they are not filterable and only readable with Load.

    /**
     * True when the DXGI format, if not uncompressing, consists of blocks, so copy regions must
     * align to the block size.
     */
    bool is_block_compressed;
    /**
     * Format used when the regular host format is block compressed but the size is not block
     * aligned, as Direct3D on PC cannot create such textures. Supports unsigned normalized
     * formats only.
     */
    DXGI_FORMAT dxgi_format_uncompressed;
    LoadShaderIndex load_shader_decompress;

    /** Maps Xenos swizzle components to DXGI format components. */
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

    /**
     * For bindful, indices in the non shader visible descriptor cache, copied to the shader
     * visible heap. For bindless, indices in the global shader visible descriptor heap.
     */
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
    /** Indices of texture and texture_signed from FindOrCreateTextureDescriptor. */
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
    /** Marks a UAV barrier pending after a write through a UAV. */
    void SetUAVBarrierPending() {
      if (resource_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        uav_barrier_pending_ = true;
      }
    }
    /** Clears the pending UAV barrier after an aliasing barrier, which is stronger. */
    void ClearUAVBarrierPending() { uav_barrier_pending_ = false; }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12_RESOURCE_STATES resource_state_;
    bool uav_barrier_pending_ = false;
  };

  bool Initialize();

  /**
   * Reports whether decompression is needed on the host, as Direct3D on PC only creates block
   * compressed textures with 4x4 aligned dimensions.
   *
   * @param format Texture format.
   * @param width Width in texels.
   * @param height Height in texels.
   * @return True when decompression is needed.
   */
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
   * Returns the index of an existing or newly created non shader visible cached descriptor (for
   * bindful) or shader visible global descriptor (for bindless).
   *
   * @param texture Texture to describe.
   * @param dimension Data dimension of the view.
   * @param is_signed True for the signed version.
   * @param host_swizzle Host swizzle of the view.
   * @return The descriptor index, or UINT32_MAX if creation failed.
   */
  u32 FindOrCreateTextureDescriptor(D3D12Texture& texture, xenos::DataDimension dimension,
                                    bool is_signed, u32 host_swizzle);
  void ReleaseTextureDescriptor(u32 descriptor_index) noexcept;
  D3D12_CPU_DESCRIPTOR_HANDLE GetTextureDescriptorCPUHandle(u32 descriptor_index) const;

  /**
   * Returns the scaled resolve buffer count. Any range up to 1 GB is accessible through 1 or 2
   * buffers. Buffer N has an offset of N GB in the scaled resolve address space. With 2x2 scale
   * 1 buffer covers 2 GB, with 3x3 scale 4 buffers cover 4.5 GB.
   *
   * @return The number of buffers.
   */
  size_t GetScaledResolveBufferCount() const {
    REX_ASSERT(IsDrawResolutionScaled());
    u64 address_space_size = static_cast<u64>(SharedMemory::kBufferSize) *
                             (u64{GetDrawResolutionScaleX()} * GetDrawResolutionScaleY());
    return static_cast<size_t>((address_space_size - 1) >> 30);
  }
  /**
   * Returns the indices of the 2 scaled resolve virtual buffers the location may be accessible
   * through. They are the same near the beginning or end of the address space, where 1 buffer
   * covers the location.
   *
   * @param address_scaled Scaled address of the location.
   * @return The 2 buffer indices.
   */
  std::array<size_t, 2> GetPossibleScaledResolveBufferIndices(u64 address_scaled) const {
    REX_ASSERT(IsDrawResolutionScaled());
    size_t address_gb = static_cast<size_t>(address_scaled >> 30);
    size_t max_index = GetScaledResolveBufferCount() - 1;
    return std::array<size_t, 2>{
        std::min(address_gb, max_index),
        std::min(std::max(address_gb, static_cast<size_t>(1)) - static_cast<size_t>(1), max_index)};
  }
  /**
   * Returns the index of the current scaled resolve buffer, which is its offset in
   * gigabytes from the start of the scaled physical memory address space.
   *
   * @return The buffer index.
   */
  size_t GetCurrentScaledResolveBufferIndex() const {
    return scaled_resolve_1gb_buffer_indices_[scaled_resolve_current_range_start_scaled_ >> 30];
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
  /** Null SRV descriptors by NullSRVDescriptorIndex, not shader visible and used for copying. */
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
   * Tiled buffers for resolved data with resolution scaling. Typed SRV or UAV creation fails for
   * offsets above 4 GB on Direct3D 12 (at least on Windows 10 2004), so a single tiled 4.5 GB
   * buffer cannot serve 3x3 scaling. Sliding window buffers instead access a single range of up
   * to 1 GB, or up to 2 GB depending on the low bits, at a time. 2 GB is the scaled
   * physical memory size at 2x scale, and older Intel GPUs support only 31 virtual address bits
   * per resource. The index is the first gigabyte. Only buffers containing over 1 GB are kept,
   * as the rest is contained in another. The array is sized with kMaxDrawResolutionScaleAlongAxis
   * while GetScaledResolveBufferCount uses the current draw resolution scale, so they match only
   * at the maximum scale.
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
  /** Last owning buffer index for each 1 GB, for aliasing barrier placement. */
  size_t scaled_resolve_1gb_buffer_indices_[(static_cast<u64>(SharedMemory::kBufferSize) *
                                                 kMaxDrawResolutionScaleAlongAxis *
                                                 kMaxDrawResolutionScaleAlongAxis +
                                             ((static_cast<u32>(1) << 30) - 1)) >>
                                            30];
  /** Range from the last successful UpdateScaledResolveRange call. */
  u64 scaled_resolve_current_range_start_scaled_;
  u64 scaled_resolve_current_range_length_scaled_;
};

}  // namespace rex::gpu::d3d12
