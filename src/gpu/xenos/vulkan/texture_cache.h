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
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gpu/xenos/pipeline/texture/cache.h"
#include "gpu/xenos/vulkan/shader.h"
#include "gpu/xenos/vulkan/shared_memory.h"
#include <rex/hash.h>
#include "gpu/xenos/host/vulkan/mem_alloc.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor;

class VulkanTextureCache final : public TextureCache {
  struct Key {};

 public:
  explicit VulkanTextureCache(Key, const xenos::RegisterFile& register_file,
                              VulkanSharedMemory& shared_memory, u32 draw_resolution_scale_x,
                              u32 draw_resolution_scale_y,
                              VulkanCommandProcessor& command_processor,
                              VkPipelineStageFlags guest_shader_pipeline_stages);

  /** Sampler parameters convertible to a host sampler or compared to check sampler bindings. */
  union SamplerParameters {
    u32 value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      xenos::BorderColor border_color : 2;
      u32 mag_linear : 1;
      u32 min_linear : 1;
      u32 mip_linear : 1;
      xenos::AnisoFilter aniso_filter : 3;
      u32 mip_min_level : 4;
      /** The texture holds the maximum mip level, so fetching is limited to mip_min_level. */
      u32 mip_base_map : 1;
    };

    SamplerParameters() : value(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<u32>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  // Transient descriptor set layouts must be initialized in the command processor first.
  static std::unique_ptr<VulkanTextureCache> Create(
      const xenos::RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages) {
    auto texture_cache = std::make_unique<VulkanTextureCache>(
        Key{}, register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y,
        command_processor, guest_shader_pipeline_stages);
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return texture_cache;
  }

  ~VulkanTextureCache();

  void BeginSubmission(u64 new_submission_index) override;
  void BeginFrame() override;
  void EndFrame();

  /**
   * Creates and untiles the textures shaders need and enqueues their transition to the sampled
   * usage. Runs within a frame. May bind compute pipelines, notifying the command processor, and
   * may insert deferred barriers before flushing the barriers preceding host GPU work.
   *
   * @param used_texture_mask Mask of used fetch constants.
   */
  void RequestTextures(u32 used_texture_mask) override;

  VkImageView GetActiveBindingOrNullImageView(u32 fetch_constant_index,
                                              xenos::FetchOpDimension dimension, bool is_signed);

  SamplerParameters GetSamplerParameters(const VulkanShader::SamplerBinding& binding) const;

  /**
   * Returns the sampler and updates its last usage submission index. Needs an open submission and
   * runs at least once per submission for every used sampler.
   *
   * @param parameters Sampler parameters.
   * @param has_overflown_out Receives true on a sampler overflow in a pending submission.
   * @return The sampler, or VK_NULL_HANDLE on overflow or on failure to create it.
   */
  VkSampler UseSampler(SamplerParameters parameters, bool& has_overflown_out);
  /**
   * Returns the submission index to await on sampler count overflow so samplers can be freed. It
   * may be the current submission, which then must be ended with a new one started.
   *
   * @param overflowed_sampler_count Number of samplers over the limit.
   * @return The submission index to await.
   */
  u64 GetSubmissionToAwaitOnSamplerOverflow(u32 overflowed_sampler_count) const;

  /**
   * Returns the 2D view of the front buffer texture for fragment shader reading, with the barrier
   * pushed in the command processor when needed. May call LoadTextureData. The red and blue swap
   * flag reports a guest to host swizzle that swaps red and blue with green preserved, which the
   * presentation fallback path needs on devices without imageViewFormatSwizzle.
   *
   * @param width_scaled_out Receives the scaled width.
   * @param height_scaled_out Receives the scaled height.
   * @param format_out Receives the texture format.
   * @param width_unscaled_out Receives the unscaled width when not null.
   * @param height_unscaled_out Receives the unscaled height when not null.
   * @param swap_source_needs_rb_swap_out Receives whether red and blue are swapped, when not null.
   * @return The view, or VK_NULL_HANDLE on failure.
   */
  VkImageView RequestSwapTexture(u32& width_scaled_out, u32& height_scaled_out,
                                 xenos::TextureFormat& format_out,
                                 u32* width_unscaled_out = nullptr,
                                 u32* height_unscaled_out = nullptr,
                                 bool* swap_source_needs_rb_swap_out = nullptr);

  bool GetScaledResolveRange(u32 start_unscaled, u32 length_unscaled,
                             u32 length_scaled_alignment_log2, u64& start_scaled_out,
                             u64& length_scaled_out) const;
  bool CommitScaledResolveRange(u32 start_unscaled, u32 length_unscaled,
                                u32 length_scaled_alignment_log2 = 0) {
    return EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                              length_scaled_alignment_log2);
  }
  VkBuffer scaled_resolve_buffer() const { return scaled_resolve_buffer_; }
  void UseScaledResolveBufferForRead();
  void UseScaledResolveBufferForWrite(u64 written_start_scaled, u64 written_length_scaled);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  u32 GetHostFormatSwizzle(TextureKey key) const override;

  u32 GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  u32 GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  bool EnsureScaledResolveMemoryCommitted(u32 start_unscaled, u32 length_unscaled,
                                          u32 length_scaled_alignment_log2 = 0) override;

  /**
   * Loads the texture data from resident memory. When the packed mip tail is level 0, the base
   * and the mips would both be on level 0 but stored separately, so the load loop counter then
   * counts the base tail and the mips tail instead of the stored level.
   *
   * @param texture Texture to load.
   * @param load_base Whether to load the base level.
   * @param load_mips Whether to load the mips.
   * @return True when the load was queued.
   */
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

  void UpdateTextureBindingsImpl(u32 fetch_constant_mask) override;

 private:
  enum LoadDescriptorSetIndex : u8 {
    kLoadDescriptorSetIndexDestination,
    kLoadDescriptorSetIndexSource,
    kLoadDescriptorSetCount,
  };

  struct HostFormat {
    LoadShaderIndex load_shader;
    /** Integer formats are not filterable and only work with ImageFetch, so none belong here. */
    VkFormat format = VK_FORMAT_UNDEFINED;
    /** True when block compressed on the host with the guest block size, kept compressed. */
    bool block_compressed = false;

    bool linear_filterable = false;
  };

  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    /** Maps Xenos swizzle components to Vulkan format components. */
    u32 swizzle = 0;
    /**
     * True when the unsigned and signed formats are compatible for one image and its data,
     * including imageViewFormatReinterpretation on portability subset devices.
     */
    bool unsigned_signed_compatible = false;
  };

  class VulkanTexture final : public Texture {
   public:
    enum class Usage : u8 {
      kUndefined,
      kTransferDestination,
      kGuestShaderSampled,
      kSwapSampled,
    };

    /**
     * Takes ownership of the image and its memory.
     *
     * @param texture_cache Owning texture cache.
     * @param key Texture key.
     * @param image Image to own.
     * @param allocation Allocation backing the image.
     * @param track_usage Whether usage transitions are tracked.
     */
    explicit VulkanTexture(VulkanTextureCache& texture_cache, const TextureKey& key, VkImage image,
                           VmaAllocation allocation, bool track_usage = true);
    ~VulkanTexture();

    VkImage image() const { return image_; }

    /**
     * Sets the usage without a transition, the caller inserts the barrier.
     *
     * @param new_usage Usage to set.
     * @return The previous usage.
     */
    Usage SetUsage(Usage new_usage) {
      Usage old_usage = usage_;
      usage_ = new_usage;
      return old_usage;
    }

    VkImageView GetView(bool is_signed, u32 host_swizzle, bool is_array = true);
    VkImageView GetOrCreate3DAs2DImageView(bool is_signed, u32 host_swizzle);

   private:
    union ViewKey {
      u32 key;
      struct {
        u32 is_signed_separate_view : 1;
        u32 host_swizzle : 12;
        u32 is_array : 1;
      };

      ViewKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

      struct Hasher {
        size_t operator()(const ViewKey& key) const {
          return std::hash<decltype(key.key)>{}(key.key);
        }
      };
      bool operator==(const ViewKey& other_key) const { return key == other_key.key; }
      bool operator!=(const ViewKey& other_key) const { return !(*this == other_key); }
    };

    static constexpr VkComponentSwizzle GetComponentSwizzle(u32 texture_swizzle,
                                                            u32 component_index) {
      xenos::TextureSwizzle texture_component_swizzle =
          xenos::TextureSwizzle((texture_swizzle >> (3 * component_index)) & 0b111);
      if (texture_component_swizzle == xenos::TextureSwizzle(component_index)) {
        // The portability subset requires IDENTITY here, not R, G, B, A.
        return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
      switch (texture_component_swizzle) {
        case xenos::REXGPU_SWIZZLE_R:
          return VK_COMPONENT_SWIZZLE_R;
        case xenos::REXGPU_SWIZZLE_G:
          return VK_COMPONENT_SWIZZLE_G;
        case xenos::REXGPU_SWIZZLE_B:
          return VK_COMPONENT_SWIZZLE_B;
        case xenos::REXGPU_SWIZZLE_A:
          return VK_COMPONENT_SWIZZLE_A;
        case xenos::REXGPU_SWIZZLE_0:
          return VK_COMPONENT_SWIZZLE_ZERO;
        case xenos::REXGPU_SWIZZLE_1:
          return VK_COMPONENT_SWIZZLE_ONE;
        default:
          return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
    }

    VkImage image_;
    VmaAllocation allocation_;

    Usage usage_ = Usage::kUndefined;

    std::unordered_map<ViewKey, VkImageView, ViewKey::Hasher> views_;
    std::unique_ptr<VulkanTexture> texture_3d_as_2d_;
    VkImageView image_view_3d_as_2d_unsigned_ = VK_NULL_HANDLE;
    VkImageView image_view_3d_as_2d_signed_ = VK_NULL_HANDLE;
  };

  struct VulkanTextureBinding {
    VkImageView image_view_unsigned;
    VkImageView image_view_signed;

    VulkanTextureBinding() { Reset(); }

    void Reset() {
      image_view_unsigned = VK_NULL_HANDLE;
      image_view_signed = VK_NULL_HANDLE;
    }
  };

  struct Sampler {
    VkSampler sampler;
    bool uses_custom_border_color;
    u64 last_usage_submission;
    std::pair<const SamplerParameters, Sampler>* used_previous;
    std::pair<const SamplerParameters, Sampler>* used_next;
  };

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
   * Creates the host formats, load pipelines, null images and sampler limit. Takes every sampler
   * allocation slot of the VkDevice except 16 for overlay use and the UI samplers, as this is the
   * only texture cache on it.
   *
   * @return True when initialization succeeded.
   */
  bool Initialize();
  bool InitializeScaledResolveBuffer();
  void ShutdownScaledResolveBuffer();

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;

  void GetTextureUsageMasks(VulkanTexture::Usage usage, VkPipelineStageFlags& stage_mask,
                            VkAccessFlags& access_mask, VkImageLayout& layout) const;
  bool EnsureScaledResolveBufferAllocated(u64 start_scaled, u64 length_scaled);
  void GetScaledResolveUsageMasks(VkPipelineStageFlags& stage_mask_out,
                                  VkAccessFlags& access_mask_out, bool write) const;

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // VMA suballocates because some drivers limit the allocation count to as low as 4096.
  VmaAllocator vma_allocator_ = VK_NULL_HANDLE;

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  static const HostFormatPair kHostFormatDXT1Unaligned;
  static const HostFormatPair kHostFormatDXT2_3Unaligned;
  static const HostFormatPair kHostFormatDXT4_5Unaligned;
  static const HostFormatPair kHostFormatDXNUnaligned;
  static const HostFormatPair kHostFormatDXT5AUnaligned;
  HostFormatPair host_formats_[64];

  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_scaled_{};

  std::array<VkDeviceMemory, 2> null_images_memory_{};
  VkImage null_image_2d_array_cube_ = VK_NULL_HANDLE;
  VkImage null_image_3d_ = VK_NULL_HANDLE;
  VkImageView null_image_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView null_image_view_cube_ = VK_NULL_HANDLE;
  VkImageView null_image_view_3d_ = VK_NULL_HANDLE;
  bool null_images_cleared_ = false;

  std::array<VulkanTextureBinding, xenos::kTextureFetchConstantCount> vulkan_texture_bindings_;

  enum : u8 {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  u8 unsupported_format_features_used_[64] = {};

  u32 sampler_max_count_;

  xenos::AnisoFilter max_anisotropy_;

  std::unordered_map<SamplerParameters, Sampler, SamplerParameters::Hasher> samplers_;
  std::pair<const SamplerParameters, Sampler>* sampler_used_first_ = nullptr;
  std::pair<const SamplerParameters, Sampler>* sampler_used_last_ = nullptr;
  u32 custom_border_color_sampler_count_ = 0;

  VkBuffer scaled_resolve_buffer_ = VK_NULL_HANDLE;
  u64 scaled_resolve_buffer_size_ = 0;
  bool scaled_resolve_buffer_sparse_ = false;
  u32 scaled_resolve_buffer_memory_type_ = UINT32_MAX;
  std::vector<VkDeviceMemory> scaled_resolve_buffer_memory_;
  u32 scaled_resolve_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<u64> scaled_resolve_sparse_allocated_;
  bool scaled_resolve_last_usage_write_ = false;
  std::pair<u64, u64> scaled_resolve_last_written_range_{0, 0};
};

}  // namespace rex::gpu::vulkan
