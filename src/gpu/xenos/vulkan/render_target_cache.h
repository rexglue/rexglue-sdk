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
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include "gpu/xenos/pipeline/render_target/cache.h"
#include "gpu/xenos/vulkan/shared_memory.h"
#include "gpu/xenos/vulkan/texture_cache.h"
#include <rex/xenos/xenos.h>
#include <rex/hash.h>
#include "gpu/xenos/host/vulkan/single_layout_descriptor_set_pool.h"
#include "gpu/xenos/host/vulkan/upload_buffer_pool.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor;

class VulkanRenderTargetCache final : public RenderTargetCache {
 public:
  union RenderPassKey {
    struct {
      /**
       * Stays 2x when emulating 2x as 4x, so guest decisions can use it. Dependencies cannot be
       * shared between 2x as 4x and true 4x passes, because framebuffers use different render
       * targets and pipelines exclude 2 samples from the sample mask. Without attachments it
       * carries the sample count between subsystems, and separate render passes keep draws
       * with different rasterization sample counts apart when variableMultisampleRate is
       * unsupported.
       */
      xenos::MSAASamples msaa_samples : xenos::kMSAASamplesBits;
      /** Bit 0 is depth, bits 1 to 4 are color. */
      u32 depth_and_color_used : 1 + xenos::kMaxColorRenderTargets;
      /**
       * 0 for unused attachments. Must be kD24FS8 even for kD24S8 when
       * VK_FORMAT_D24_UNORM_S8_UINT is not supported.
       */
      xenos::DepthRenderTargetFormat depth_format : xenos::kDepthRenderTargetFormatBits;
      /** Linear or sRGB included if host sRGB is used. */
      xenos::ColorRenderTargetFormat color_0_view_format : xenos::kColorRenderTargetFormatBits;
      xenos::ColorRenderTargetFormat color_1_view_format : xenos::kColorRenderTargetFormatBits;
      xenos::ColorRenderTargetFormat color_2_view_format : xenos::kColorRenderTargetFormatBits;
      xenos::ColorRenderTargetFormat color_3_view_format : xenos::kColorRenderTargetFormatBits;
      u32 color_rts_use_transfer_formats : 1;
      u32 padding : 7;
    };
    u32 key = 0;
    struct Hasher {
      size_t operator()(const RenderPassKey& key) const { return std::hash<u32>{}(key.key); }
    };
    bool operator==(const RenderPassKey& other_key) const { return key == other_key.key; }
    bool operator!=(const RenderPassKey& other_key) const { return !(*this == other_key); }
    bool operator<(const RenderPassKey& other_key) const { return key < other_key.key; }
  };
  static_assert(std::is_trivially_copyable_v<RenderPassKey> &&
                std::has_unique_object_representations_v<RenderPassKey>);
  REX_STATIC_ASSERT_SIZE(RenderPassKey, sizeof(u32));

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D host_extent{};
    Framebuffer() = default;
    Framebuffer(VkFramebuffer framebuffer, const VkExtent2D& host_extent)
        : framebuffer(framebuffer), host_extent(host_extent) {}
  };

  VulkanRenderTargetCache(const xenos::RegisterFile& register_file, const runtime::Memory& memory,
                          u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                          VulkanCommandProcessor& command_processor);
  ~VulkanRenderTargetCache();

  /**
   * Creates the render target cache. Transient descriptor set layouts must be initialized in the
   * command processor first. The fragment shader interlock path needs sample interlock with
   * common sample coverage (more is acceptable), fragmentStoresAndAtomics, sampleRateShading
   * (some SPIR-V revisions need it to access the sample mask input), standardSampleLocations
   * (to calculate depth at the samples), and enough storage buffer slots for the shared memory
   * buffers and the EDRAM buffer. Interlock orders fragments only within a subpass, so the render
   * pass has external by region dependencies to keep interlocking between passes. Framebuffer
   * global dependencies use explicit barriers when EDRAM addressing relative to the fragment
   * coordinates changes.
   *
   * @param shared_memory_binding_count Number of shared memory bindings.
   * @return True on success.
   */
  bool Initialize(u32 shared_memory_binding_count);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();

  Path GetPath() const override { return path_; }

  VkBuffer edram_buffer() const { return edram_buffer_; }

  /**
   * Performs the resolve to a shared memory area according to the current register values and
   * clears the render targets if needed. Must be in a frame.
   *
   * @param memory Guest memory.
   * @param shared_memory Shared memory the resolve writes to.
   * @param texture_cache Texture cache used while resolving.
   * @param written_address_out Receives the written address, 0 when nothing was written.
   * @param written_length_out Receives the written length, 0 when nothing was written.
   * @return True on success.
   */
  bool Resolve(const runtime::Memory& memory, VulkanSharedMemory& shared_memory,
               VulkanTextureCache& texture_cache, u32& written_address_out,
               u32& written_length_out);

  bool Update(bool is_rasterization_done, xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
              u32 normalized_color_mask, const Shader& vertex_shader) override;
  /** Binding information for the last successful update. */
  RenderPassKey last_update_render_pass_key() const { return last_update_render_pass_key_; }
  VkRenderPass last_update_render_pass() const { return last_update_render_pass_; }
  const Framebuffer* last_update_framebuffer() const { return last_update_framebuffer_; }
  void GetLastUpdateRenderingAttachments(VkRenderingAttachmentInfo* color_attachments,
                                         u32* color_attachment_count_out,
                                         VkRenderingAttachmentInfo* depth_attachment,
                                         VkRenderingAttachmentInfo* stencil_attachment) const;

  /**
   * Reports whether R16G16_SNORM is used, which covers -1 to 1 rather than the needed -32 to 32.
   * Persistent data does not depend on it, so per title configuration may override it.
   *
   * @return True when R16G16 is truncated to -1 to 1.
   */
  bool IsFixedRG16TruncatedToMinus1To1() const;
  /**
   * Reports whether R16G16B16A16_SNORM is used, which covers -1 to 1 rather than the needed -32
   * to 32. Persistent data does not depend on it, so per title configuration may override it.
   *
   * @return True when R16G16B16A16 is truncated to -1 to 1.
   */
  bool IsFixedRGBA16TruncatedToMinus1To1() const;
  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  bool depth_unorm24_vulkan_format_supported() const {
    return depth_unorm24_vulkan_format_supported_;
  }
  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  bool msaa_2x_attachments_supported() const { return msaa_2x_attachments_supported_; }
  bool msaa_2x_no_attachments_supported() const { return msaa_2x_no_attachments_supported_; }
  bool IsMSAA2xSupported(bool subpass_has_attachments) const {
    return subpass_has_attachments ? msaa_2x_attachments_supported_
                                   : msaa_2x_no_attachments_supported_;
  }

  /**
   * Returns the render pass object. A render pass managed by the render target cache may be ended
   * and resumed at any time, to allow copying and texture loading.
   *
   * @param key Render pass key.
   * @return The render pass, or VK_NULL_HANDLE if failed to create.
   */
  VkRenderPass GetHostRenderTargetsRenderPass(RenderPassKey key);
  VkRenderPass GetFragmentShaderInterlockRenderPass() const {
    REX_ASSERT(GetPath() == Path::kPixelShaderInterlock);
    return fsi_render_pass_;
  }

  VkFormat GetDepthVulkanFormat(xenos::DepthRenderTargetFormat format) const;
  VkFormat GetColorVulkanFormat(xenos::ColorRenderTargetFormat format) const;
  /**
   * Returns the format used for ownership transfers. Integer formats are preferred because
   * floating point NaNs must be propagated without changing the bit representation and SNORM has
   * 2 representations of -1.
   *
   * @param format Color render target format.
   * @param msaa_samples Guest MSAA sample count.
   * @param is_integer_out Receives whether the returned format is an integer one.
   * @return The Vulkan transfer format.
   */
  VkFormat GetColorOwnershipTransferVulkanFormat(
      xenos::ColorRenderTargetFormat format,
      xenos::MSAASamples msaa_samples = xenos::MSAASamples::k1X,
      bool* is_integer_out = nullptr) const;

 protected:
  u32 GetMaxRenderTargetWidth() const override;
  u32 GetMaxRenderTargetHeight() const override;

  std::unique_ptr<RenderTarget> CreateRenderTarget(RenderTargetKey key) override;

  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

  bool IsGammaFormatHostStorageSeparate() const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  /**
   * Usage of the EDRAM buffer. Fragment and compute usages are never combined. With host render
   * targets the usual sequence is optional compute writes and optional fragment reads (host depth
   * copy storing for EDRAM range ownership transfers), compute writes (copying from host render
   * targets during resolve), then compute reads (writing to shared memory during resolve). With
   * fragment shader interlock it is fragment reads and writes (depth, stencil and color
   * operations), then compute reads. Fragment reads and compute reads normally do not follow
   * each other, so a wider barrier in the normal scenario would not pay off.
   */
  enum class EDRAMBufferUsage : u8 {
    /** Host depth copy storing. */
    kFragmentRead,
    /** Fragment shader interlock depth, stencil and color operations. */
    kFragmentReadWrite,
    /** Resolve copying to the shared memory. */
    kComputeRead,
    /** Resolve copying from host render targets. */
    kComputeWrite,
    kTransferRead,
    kTransferWrite,
  };

  /** Values are ordered by how strong the barrier conditions are. */
  enum class EDRAMBufferModificationStatus : u8 {
    /** No uncommitted shader writes. */
    kUnmodified,
    /** Must commit before the next fragment shader interlock usage with overlap. */
    kViaFragmentShaderInterlock,
    /** Must commit before any next fragment shader interlock usage. */
    kViaUnordered,
  };

  enum ResolveCopyDescriptorSet : u8 {
    /** Never changes. */
    kResolveCopyDescriptorSetEDRAM,
    /** Shared memory or a region in it. */
    kResolveCopyDescriptorSetDest,

    kResolveCopyDescriptorSetCount,
  };

  struct ResolveCopyShaderCode {
    const u32* unscaled;
    size_t unscaled_size_bytes;
    const u32* scaled;
    size_t scaled_size_bytes;
  };

  static void GetEDRAMBufferUsageMasks(EDRAMBufferUsage usage, VkPipelineStageFlags& stage_mask_out,
                                       VkAccessFlags& access_mask_out);
  bool IsColor16FormatFloatLike(xenos::ColorRenderTargetFormat format) const;
  void UseEDRAMBuffer(EDRAMBufferUsage new_usage);
  void MarkEDRAMBufferModified(EDRAMBufferModificationStatus modification_status =
                                   EDRAMBufferModificationStatus::kViaUnordered);
  void CommitEDRAMBufferShaderWrites(
      EDRAMBufferModificationStatus commit_status =
          EDRAMBufferModificationStatus::kViaFragmentShaderInterlock);

  VulkanCommandProcessor& command_processor_;

  Path path_ = Path::kHostRenderTargets;

  // Accessible in fragment and compute shaders.
  VkDescriptorSetLayout descriptor_set_layout_storage_buffer_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_x2_ = VK_NULL_HANDLE;

  std::unique_ptr<gpu::vulkan::SingleLayoutDescriptorSetPool> descriptor_set_pool_sampled_image_;
  std::unique_ptr<gpu::vulkan::SingleLayoutDescriptorSetPool> descriptor_set_pool_sampled_image_x2_;

  VkDeviceMemory edram_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer edram_buffer_ = VK_NULL_HANDLE;
  EDRAMBufferUsage edram_buffer_usage_;
  EDRAMBufferModificationStatus edram_buffer_modification_status_ =
      EDRAMBufferModificationStatus::kUnmodified;
  VkDescriptorPool edram_storage_buffer_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet edram_storage_buffer_descriptor_set_;

  VkPipelineLayout resolve_copy_pipeline_layout_ = VK_NULL_HANDLE;
  static const ResolveCopyShaderCode
      kResolveCopyShaders[static_cast<size_t>(draw_state::ResolveCopyShaderIndex::kCount)];
  std::array<VkPipeline, static_cast<size_t>(draw_state::ResolveCopyShaderIndex::kCount)>
      resolve_copy_pipelines_{};

  /** On the fragment shader interlock path it only passes parameters to pipeline setup. */
  RenderPassKey last_update_render_pass_key_;
  VkRenderPass last_update_render_pass_ = VK_NULL_HANDLE;
  /** Not used on the fragment shader interlock path. */
  u32 last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  /** Not used on the fragment shader interlock path. */
  const RenderTarget* last_update_framebuffer_attachments_[1 + xenos::kMaxColorRenderTargets] = {};
  const Framebuffer* last_update_framebuffer_ = VK_NULL_HANDLE;

  /** Must outlive the framebuffers referencing it. */
  class VulkanRenderTarget final : public RenderTarget {
   public:
    static constexpr VkPipelineStageFlags kColorDrawStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    static constexpr VkAccessFlags kColorDrawAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kColorDrawLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    static constexpr VkPipelineStageFlags kDepthDrawStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    static constexpr VkAccessFlags kDepthDrawAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kDepthDrawLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    /**
     * Takes ownership of the Vulkan objects passed to the constructor.
     *
     * @param key Render target key.
     * @param render_target_cache Owning cache.
     * @param image Image backing the render target.
     * @param memory Memory bound to the image.
     * @param view_depth_color Depth or color view.
     * @param view_depth_stencil Optional depth and stencil view.
     * @param view_stencil Optional stencil view.
     * @param view_srgb Optional sRGB view.
     * @param view_color_transfer_separate Optional color view for transfers.
     * @param descriptor_set_index_transfer_source Index of the transfer source descriptor set.
     */
    VulkanRenderTarget(RenderTargetKey key, VulkanRenderTargetCache& render_target_cache,
                       VkImage image, VkDeviceMemory memory, VkImageView view_depth_color,
                       VkImageView view_depth_stencil, VkImageView view_stencil,
                       VkImageView view_srgb, VkImageView view_color_transfer_separate,
                       size_t descriptor_set_index_transfer_source)
        : RenderTarget(key),
          render_target_cache_(render_target_cache),
          image_(image),
          memory_(memory),
          view_depth_color_(view_depth_color),
          view_depth_stencil_(view_depth_stencil),
          view_stencil_(view_stencil),
          view_srgb_(view_srgb),
          view_color_transfer_separate_(view_color_transfer_separate),
          descriptor_set_index_transfer_source_(descriptor_set_index_transfer_source) {}
    ~VulkanRenderTarget();

    VkImage image() const { return image_; }

    VkImageView view_depth_color() const { return view_depth_color_; }
    VkImageView view_depth_stencil() const { return view_depth_stencil_; }
    VkImageView view_color_transfer_separate() const { return view_color_transfer_separate_; }
    VkImageView view_color_transfer() const {
      return view_color_transfer_separate_ != VK_NULL_HANDLE ? view_color_transfer_separate_
                                                             : view_depth_color_;
    }
    VkDescriptorSet GetDescriptorSetTransferSource() const {
      gpu::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
          key().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
                         : *render_target_cache_.descriptor_set_pool_sampled_image_;
      return descriptor_set_pool.Get(descriptor_set_index_transfer_source_);
    }

    static void GetDrawUsage(bool is_depth, VkPipelineStageFlags* stage_mask_out,
                             VkAccessFlags* access_mask_out, VkImageLayout* layout_out) {
      if (stage_mask_out) {
        *stage_mask_out = is_depth ? kDepthDrawStageMask : kColorDrawStageMask;
      }
      if (access_mask_out) {
        *access_mask_out = is_depth ? kDepthDrawAccessMask : kColorDrawAccessMask;
      }
      if (layout_out) {
        *layout_out = is_depth ? kDepthDrawLayout : kColorDrawLayout;
      }
    }
    void GetDrawUsage(VkPipelineStageFlags* stage_mask_out, VkAccessFlags* access_mask_out,
                      VkImageLayout* layout_out) const {
      GetDrawUsage(key().is_depth, stage_mask_out, access_mask_out, layout_out);
    }
    VkPipelineStageFlags current_stage_mask() const { return current_stage_mask_; }
    VkAccessFlags current_access_mask() const { return current_access_mask_; }
    VkImageLayout current_layout() const { return current_layout_; }
    void SetUsage(VkPipelineStageFlags stage_mask, VkAccessFlags access_mask,
                  VkImageLayout layout) {
      current_stage_mask_ = stage_mask;
      current_access_mask_ = access_mask;
      current_layout_ = layout;
    }

    u32 temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(u32 index) { temporary_sort_index_ = index; }

   private:
    VulkanRenderTargetCache& render_target_cache_;

    VkImage image_;
    VkDeviceMemory memory_;

    VkImageView view_depth_color_;
    /** Optional views. */
    VkImageView view_depth_stencil_;
    VkImageView view_stencil_;
    VkImageView view_srgb_;
    VkImageView view_color_transfer_separate_;

    /** 2 sampled images for depth and stencil, 1 sampled image for color. */
    size_t descriptor_set_index_transfer_source_;

    VkPipelineStageFlags current_stage_mask_ = 0;
    VkAccessFlags current_access_mask_ = 0;
    VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    u32 temporary_sort_index_ = 0;
  };

  struct FramebufferKey {
    RenderPassKey render_pass_key;

    /** Same as RenderTargetKey::pitch_tiles_at_32bpp. */
    u32 pitch_tiles_at_32bpp : 8;
    /** Range is [0, 2047]. */
    u32 depth_base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 color_0_base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 padding_0 : 2;

    u32 color_1_base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 color_2_base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 padding_1 : 10;

    u32 color_3_base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 padding_2 : 21;

    FramebufferKey() { Reset(); }
    bool operator==(const FramebufferKey& key) const {
      return std::memcmp(this, &key, sizeof(*this)) == 0;
    }
    using Hasher = rex::XXHasher<FramebufferKey>;
    void Reset() { std::memset(this, 0, sizeof(*this)); }
  };
  static_assert(std::is_trivially_copyable_v<FramebufferKey> &&
                std::has_unique_object_representations_v<FramebufferKey>);

  /** Ordered from the least to the most frequently changed. */
  enum TransferUsedDescriptorSet : u8 {
    kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTextures,
    /** Mutually exclusive with kTransferUsedDescriptorSetDepthStencilTextures. */
    kTransferUsedDescriptorSetColorTexture,

    kTransferUsedDescriptorSetCount,

    kTransferUsedDescriptorSetHostDepthBufferBit = static_cast<u32>(1)
                                                   << kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTexturesBit =
        static_cast<u32>(1) << kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTexturesBit =
        static_cast<u32>(1) << kTransferUsedDescriptorSetDepthStencilTextures,
    kTransferUsedDescriptorSetColorTextureBit = static_cast<u32>(1)
                                                << kTransferUsedDescriptorSetColorTexture,
  };

  /** 32 bit push constants keep size calculation simple and avoid std140 packing issues. */
  enum TransferUsedPushConstantDword : u8 {
    kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddress,
    /** Changed 8 times per transfer. */
    kTransferUsedPushConstantDwordStencilMask,

    kTransferUsedPushConstantDwordCount,

    kTransferUsedPushConstantDwordHostDepthAddressBit =
        static_cast<u32>(1) << kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddressBit = static_cast<u32>(1)
                                               << kTransferUsedPushConstantDwordAddress,
    kTransferUsedPushConstantDwordStencilMaskBit = static_cast<u32>(1)
                                                   << kTransferUsedPushConstantDwordStencilMask,
  };

  enum class TransferPipelineLayoutIndex : u8 {
    kColor,
    kDepth,
    kColorToStencilBit,
    kDepthToStencilBit,
    kColorAndHostDepthTexture,
    kColorAndHostDepthBuffer,
    kDepthAndHostDepthTexture,
    kDepthAndHostDepthBuffer,

    kCount,
  };

  struct TransferPipelineLayoutInfo {
    u32 used_descriptor_sets;
    u32 used_push_constant_dwords;
  };

  static const TransferPipelineLayoutInfo
      kTransferPipelineLayoutInfos[static_cast<size_t>(TransferPipelineLayoutIndex::kCount)];

  // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
  enum class TransferMode : u32 {
    kColorToDepth,
    kColorToColor,

    kDepthToDepth,
    kDepthToColor,

    kColorToStencilBit,
    kDepthToStencilBit,

    /**
     * Two source mode. Uses the host depth when, converted to the guest format, it matches the
     * owner source (not modified, keeps host precision), otherwise the guest data (significantly
     * modified, possibly cleared). Stencil for FragStencilRef always comes from the guest source.
     */
    kColorAndHostDepthToDepth,
    /** Two source mode for different source and destination depth formats. */
    kDepthAndHostDepthToDepth,

    /**
     * Fetches the host depth from a copy in the EDRAM buffer when it is the same image as the
     * destination. The copy is a scratch buffer with tiles laid out linearly at the pitch of the
     * original render target, and 40 sample columns are not swapped, which only the color source
     * does.
     */
    kColorAndHostDepthCopyToDepth,
    kDepthAndHostDepthCopyToDepth,

    kCount,
  };

  enum class TransferOutput : u8 {
    kColor,
    kDepth,
    kStencilBit,
  };

  struct TransferModeInfo {
    TransferOutput output;
    TransferPipelineLayoutIndex pipeline_layout;
  };

  static const TransferModeInfo kTransferModes[static_cast<size_t>(TransferMode::kCount)];

  union TransferShaderKey {
    u32 key;
    struct {
      xenos::MSAASamples dest_msaa_samples : xenos::kMSAASamplesBits;
      u32 dest_color_rt_index : xenos::kColorRenderTargetIndexBits;
      u32 dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MSAASamples source_msaa_samples : xenos::kMSAASamplesBits;
      /**
       * Always 1x when the host depth is a copy from a buffer rather than an image, so the same
       * pipeline is not created for different sample counts.
       */
      xenos::MSAASamples host_depth_source_msaa_samples : xenos::kMSAASamplesBits;
      u32 source_resource_format : xenos::kRenderTargetFormatBits;

      static_assert(static_cast<size_t>(TransferMode::kCount) <= (static_cast<size_t>(1) << 4));
      /**
       * Last bits because this affects the pipeline layout, so after sorting it changes as few
       * times as possible. Depth buffers have an additional stencil texture.
       */
      TransferMode mode : 4;
    };

    TransferShaderKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const { return std::hash<u32>{}(key.key); }
    };
    bool operator==(const TransferShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const TransferShaderKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferShaderKey& other_key) const { return key < other_key.key; }
  };

  struct TransferPipelineKey {
    RenderPassKey render_pass_key;
    TransferShaderKey shader_key;

    TransferPipelineKey(RenderPassKey render_pass_key, TransferShaderKey shader_key)
        : render_pass_key(render_pass_key), shader_key(shader_key) {}

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        XXH3_state_t hash_state;
        XXH3_64bits_reset(&hash_state);
        XXH3_64bits_update(&hash_state, &key.render_pass_key, sizeof(key.render_pass_key));
        XXH3_64bits_update(&hash_state, &key.shader_key, sizeof(key.shader_key));
        return static_cast<size_t>(XXH3_64bits_digest(&hash_state));
      }
    };
    bool operator==(const TransferPipelineKey& other_key) const {
      return render_pass_key == other_key.render_pass_key && shader_key == other_key.shader_key;
    }
    bool operator!=(const TransferPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferPipelineKey& other_key) const {
      if (render_pass_key != other_key.render_pass_key) {
        return render_pass_key < other_key.render_pass_key;
      }
      return shader_key < other_key.shader_key;
    }
  };

  union TransferAddressConstant {
    u32 constant;
    struct {
      /** Pitches and bases are in tiles. */
      u32 dest_pitch : xenos::kEDRAMPitchTilesBits;
      u32 source_pitch : xenos::kEDRAMPitchTilesBits;
      /**
       * Destination base minus source base, not vice versa, because this transforms the
       * coordinate system rather than addresses. Has 1 extra bit as a signed difference between
       * two EDRAM bases. 0 for host_depth_source_is_copy, where it is ignored as destination
       * equals source.
       */
      i32 source_to_dest : xenos::kEDRAMBaseTilesBits + 1;
    };
    TransferAddressConstant() : constant(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(constant)); }
    bool operator==(const TransferAddressConstant& other_constant) const {
      return constant == other_constant.constant;
    }
    bool operator!=(const TransferAddressConstant& other_constant) const {
      return !(*this == other_constant);
    }
  };

  struct TransferInvocation {
    Transfer transfer;
    TransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer, const TransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other_invocation) const {
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      REX_ASSERT(transfer.source != nullptr);
      REX_ASSERT(other_invocation.transfer.source != nullptr);
      u32 source_index =
          static_cast<const VulkanRenderTarget*>(transfer.source)->temporary_sort_index();
      u32 other_source_index =
          static_cast<const VulkanRenderTarget*>(other_invocation.transfer.source)
              ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  union DumpPipelineKey {
    u32 key;
    struct {
      xenos::MSAASamples msaa_samples : 2;
      u32 resource_format : 4;
      /**
       * Last bit because this affects the pipeline, so after sorting it changes at most once.
       * Depth buffers have an additional stencil SRV.
       */
      u32 is_depth : 1;
    };

    DumpPipelineKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const DumpPipelineKey& key) const { return std::hash<u32>{}(key.key); }
    };
    bool operator==(const DumpPipelineKey& other_key) const { return key == other_key.key; }
    bool operator!=(const DumpPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const DumpPipelineKey& other_key) const { return key < other_key.key; }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      REX_ASSERT(!is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      REX_ASSERT(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
  };

  /**
   * Dumping has no strict dependency on the group size. An integer multiple of it must be 80x16
   * samples (and no larger) for 32bpp, or 40x16 samples for 64bpp, because only half of the pair
   * of tiles may need to be dumped. 8x16 is 128, the minimum required group size on Vulkan and
   * the maximum number of lanes in a subgroup on Vulkan.
   */
  static constexpr u32 kDumpSamplesPerGroupX = 8;
  static constexpr u32 kDumpSamplesPerGroupY = 16;

  union DumpPitches {
    u32 pitches;
    struct {
      /** Both in tiles. */
      u32 dest_pitch : xenos::kEDRAMPitchTilesBits;
      u32 source_pitch : xenos::kEDRAMPitchTilesBits;
    };
    DumpPitches() : pitches(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(pitches)); }
    bool operator==(const DumpPitches& other_pitches) const {
      return pitches == other_pitches.pitches;
    }
    bool operator!=(const DumpPitches& other_pitches) const { return !(*this == other_pitches); }
  };

  union DumpOffsets {
    u32 offsets;
    struct {
      /** May be beyond the EDRAM tile count when EDRAM addressing wraps, hence 1 extra bit. */
      u32 dispatch_first_tile : xenos::kEDRAMBaseTilesBits + 1;
      u32 source_base_tiles : xenos::kEDRAMBaseTilesBits;
    };
    DumpOffsets() : offsets(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(offsets)); }
    bool operator==(const DumpOffsets& other_offsets) const {
      return offsets == other_offsets.offsets;
    }
    bool operator!=(const DumpOffsets& other_offsets) const { return !(*this == other_offsets); }
  };

  enum DumpDescriptorSet : u8 {
    /**
     * Never changes. Same in both color and depth pipeline layouts and kept first for pipeline
     * layout compatibility, so it is set once.
     */
    kDumpDescriptorSetEDRAM,
    /** One resolve may need multiple sources. Layouts differ for color and depth. */
    kDumpDescriptorSetSource,

    kDumpDescriptorSetCount,
  };

  enum DumpPushConstant : u8 {
    /** May be different for different sources. */
    kDumpPushConstantPitches,
    /** May be changed multiple times for the same source. */
    kDumpPushConstantOffsets,

    kDumpPushConstantCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    DumpPipelineKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle, const DumpPipelineKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    bool operator<(const DumpInvocation& other_invocation) const {
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      REX_ASSERT(rectangle.render_target != nullptr);
      u32 render_target_index =
          static_cast<const VulkanRenderTarget*>(rectangle.render_target)->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle = other_invocation.rectangle;
      u32 other_render_target_index =
          static_cast<const VulkanRenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  struct DirectResolvePushConstants {
    draw_state::ResolveCopyShaderConstants resolve;
    u32 source_base_tiles;
    u32 source_pitch_tiles;
    u32 dispatch_first_tile;
  };

  struct DirectResolvePipelineKey {
    DumpPipelineKey dump_pipeline_key;
    draw_state::ResolveCopyShaderIndex copy_shader;
    bool draw_resolution_scaled;
    u64 packed() const {
      return static_cast<u64>(dump_pipeline_key.key) |
             (static_cast<u64>(static_cast<size_t>(copy_shader)) << 32) |
             (static_cast<u64>(draw_resolution_scaled ? 1 : 0) << 40);
    }
    struct Hasher {
      size_t operator()(const DirectResolvePipelineKey& key) const {
        return std::hash<u64>{}(key.packed());
      }
    };
    bool operator==(const DirectResolvePipelineKey& other_key) const {
      return packed() == other_key.packed();
    }
  };

  /**
   * Returns the framebuffer object.
   *
   * @param render_pass_key Render pass key.
   * @param pitch_tiles_at_32bpp Pitch in tiles at 32bpp.
   * @param depth_and_color_render_targets Depth render target followed by the color ones.
   * @return The framebuffer, or nullptr if failed to create.
   */
  const Framebuffer* GetHostRenderTargetsFramebuffer(
      RenderPassKey render_pass_key, u32 pitch_tiles_at_32bpp,
      const RenderTarget* const* depth_and_color_render_targets);

  /**
   * Returns the fragment shader module for an ownership transfer, caching it per key.
   *
   * The destination pixel index is split into a 32bpp tile and a tile relative pixel index.
   * Division by non power of two constants includes a 4 cycle 32*32 multiplication on AMD even
   * though so many bits are not needed. An OpUnreachable path for position upper bits set is not
   * eliminated when compiling for AMD via RenderDoc on Windows.
   *
   * At 4x with Vulkan standard locations the first sample bit is the horizontal sample and the
   * second is the vertical one. At native 2x the top sample is 1 and the bottom is 0, with 2x as
   * 4x the top is 0 and the bottom is 3.
   *
   * Source mapping from destination pixel and sample coordinates.
   * - 32bpp to 64bpp loads 2 source dwords, from 2 adjacent horizontal samples at 4x or from 2
   *   adjacent horizontal pixels at 1x and 2x.
   * - 4x to 4x maps the destination horizontal sample to the source horizontal pixel, with
   *   vertical samples matching.
   * - 4x to 2x reuses the pixel index. Sample 1 (native 2x) or 0 (2x as 4x) becomes samples 0 and
   *   1, sample 0 or 3 becomes samples 2 and 3.
   * - 4x to 1x reuses the horizontal pixel index and vertical pixel 1 becomes sample 2.
   * - 1x and 2x to 4x or 1x and 2x map 1 destination horizontal sample (to 4x) or pixel (to 1x
   *   and 2x) to 2 source horizontal pixels.
   * - 64bpp to 32bpp picks the half to load as well. To a 4x destination it is the horizontal
   *   sample index, with 1 destination horizontal pixel covering 1 source horizontal sample from
   *   a 4x source (2 destination horizontal pixels per source pixel, vertical pixel and second
   *   sample bit unchanged), or 1 source pixel from a 1x or 2x source.
   * - To a 1x or 2x destination the half is the horizontal pixel index. From a 4x source
   *   (destination horizontal pixel >> 1) & 1 is the horizontal sample, 2 destination
   *   horizontal pixels cover 1 source horizontal sample and 4 cover 1 source pixel. A 2x
   *   destination maps vertical samples (1 or 0 in the first bit for native 2x, 0 or 1 in the
   *   second bit for 2x as 4x) to source vertical samples (second bit), a 1x destination maps
   *   1 vertical pixel to 1 source vertical sample. From a 1x or 2x source 2 destination
   *   horizontal pixels cover 1 source pixel.
   * - Same bits per pixel from 4x to 2x maps horizontal pixels to samples and the vertical sample
   *   to the second sample bit. From 4x to 1x pixels map to samples. From 1x or 2x to 4x
   *   horizontal samples map to pixels.
   * - The vertical axis for 1x and 2x sources is mapped separately. To a 4x destination, the
   *   second sample bit becomes the 2x source vertical sample (1 or 0 for native 2x, 0 or 3 for
   *   2x as 4x) or the 1x source Y pixel. A 2x source to a 1x destination maps destination
   *   vertical pixels to source vertical samples with the same encoding. A 1x source to a 2x
   *   destination maps destination vertical samples (1 or 0 in the first bit for native 2x, 0
   *   or 1 in the second bit for 2x as 4x) to source vertical pixels.
   *
   * The source tile index is the destination tile index plus a signed difference, so it is
   * negative when the transfer crosses EDRAM addressing wrapping and is masked with
   * kEDRAMTileCount - 1 regardless of the sign. The same holds for the host depth source.
   *
   * Depth outputs use the host float32 depth when, converted to the guest format, it equals the
   * guest source (up to date), otherwise the guest depth is converted to host. The host depth
   * texture uses the mapping above for 32bpp to 32bpp only. The host depth copy buffer starts at
   * (0, 0) of the destination, does not swap 40 sample columns and does not wrap the tile index,
   * as it is used for depth to depth transfers only and written without the base offset.
   * The kD24S8 conversion to float32 rescales from 0 to 0xFFFFFF onto 0 to 0x1000000 the way
   * float division followed by multiplication does on x86-64 MSVC with default SSE rounding
   * (values from 0x800000 up grow by 1) and then biases the exponent, because multiplying by
   * 1.0 / 0xFFFFFF gives wrong results, for instance for 0xC00000.
   *
   * A 32bpp destination uses a packed value. For color it is the packed 32bpp color. For depth
   * and stencil it is stencil in bits 0 to 7 and depth in bits 8 to 31, or depth in bits 0 to 23
   * with zeros above when only depth is packed. For a stencil bit it is stencil in bits 0 to 7.
   *
   * @param key Transfer shader key.
   * @return The shader module, or VK_NULL_HANDLE if failed to create.
   */
  VkShaderModule GetTransferShader(TransferShaderKey key);
  /**
   * Returns the transfer pipelines. With sample rate shading there is 1 pipeline, without it as
   * many pipelines as there are samples.
   *
   * @param key Transfer pipeline key.
   * @return Pointer to the pipelines, or nullptr if failed to create a pipeline.
   */
  VkPipeline const* GetTransferPipelines(TransferPipelineKey key);

  /**
   * Performs ownership transfers for render targets. Each render target and vector may be null
   * or empty when there is nothing to do. resolve_clear_rectangle is expected from
   * PrepareHostRenderTargetsResolveClear, which does all the needed size bound checks.
   *
   * @param render_target_count Number of render targets.
   * @param render_targets Render targets.
   * @param render_target_transfers Transfers per render target.
   * @param render_target_resolve_clear_values Resolve clear values per render target.
   * @param resolve_clear_rectangle Rectangle to clear.
   */
  void PerformTransfersAndResolveClears(
      u32 render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const u64* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr);

  /**
   * Returns the compute pipeline copying a host render target to the EDRAM buffer.
   *
   * 64bpp tiles are
   * treated as storing 40x16 samples rather than 80x16 to keep addressing into the texture
   * simple. The destination sample index is split into a 32bpp tile and a tile relative sample
   * index. Division by non power of two constants includes a 4 cycle 32*32 multiplication on AMD
   * even though so many bits are not needed. An OpUnreachable path for position upper bits set is
   * not eliminated when compiling for AMD via RenderDoc on Windows.
   *
   * @param key Dump pipeline key.
   * @return The pipeline, or VK_NULL_HANDLE if failed to create.
   */
  VkPipeline GetDumpPipeline(DumpPipelineKey key);
  VkPipeline GetDirectResolvePipeline(DirectResolvePipelineKey key);
  bool TryResolveCopyDirectly(const draw_state::ResolveInfo& resolve_info,
                              draw_state::ResolveCopyShaderIndex copy_shader,
                              bool draw_resolution_scaled);

  /**
   * Writes contents of host render targets within rectangles from
   * ResolveInfo::GetCopyEDRAMTileSpan to edram_buffer_.
   *
   * @param dump_base First EDRAM tile to dump.
   * @param dump_row_length_used Used row length in tiles.
   * @param dump_rows Number of rows.
   * @param dump_pitch Row pitch in tiles.
   * @return True on success.
   */
  bool DumpRenderTargets(u32 dump_base, u32 dump_row_length_used, u32 dump_rows, u32 dump_pitch);

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_unorm24_vulkan_format_supported_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_attachments_supported_ = false;
  bool msaa_2x_no_attachments_supported_ = false;
  bool color_16bit_transfer_uint_formats_supported_ = true;
  bool color_32bit_transfer_uint_formats_supported_ = true;
  bool color_rg16_draw_format_fallback_to_float_ = false;
  bool color_rgba16_draw_format_fallback_to_float_ = false;

  /** VK_NULL_HANDLE if failed to create. */
  std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKey::Hasher> render_passes_;

  std::unordered_map<FramebufferKey, Framebuffer, FramebufferKey::Hasher> framebuffers_;

  /**
   * Set 0 is the EDRAM storage buffer, set 1 is the source depth sampled image (and unused
   * stencil from the transfer descriptor set). HostDepthStoreConstants go via push constants.
   */
  VkPipelineLayout host_depth_store_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline host_depth_store_pipelines_[static_cast<size_t>(xenos::MSAASamples::k4X) + 1] = {};

  std::unique_ptr<gpu::vulkan::VulkanUploadBufferPool> transfer_vertex_buffer_pool_;
  VkShaderModule transfer_passthrough_vertex_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout
      transfer_pipeline_layouts_[static_cast<size_t>(TransferPipelineLayoutIndex::kCount)] = {};
  /** VK_NULL_HANDLE if failed to create. */
  std::unordered_map<TransferShaderKey, VkShaderModule, TransferShaderKey::Hasher>
      transfer_shaders_;
  /**
   * With sample rate shading, 1 pipeline per entry, without it 1 pipeline per sample per entry.
   * VK_NULL_HANDLE if failed to create.
   */
  std::unordered_map<TransferPipelineKey, std::array<VkPipeline, 4>, TransferPipelineKey::Hasher>
      transfer_pipelines_;

  VkPipelineLayout dump_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout dump_pipeline_layout_depth_ = VK_NULL_HANDLE;
  /** Compute pipelines copying host render targets to EDRAM. VK_NULL_HANDLE if failed. */
  std::unordered_map<DumpPipelineKey, VkPipeline, DumpPipelineKey::Hasher> dump_pipelines_;
  VkPipelineLayout direct_resolve_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout direct_resolve_pipeline_layout_depth_ = VK_NULL_HANDLE;
  std::unordered_map<DirectResolvePipelineKey, VkPipeline, DirectResolvePipelineKey::Hasher>
      direct_resolve_pipelines_;

  /** Temporary storage for Resolve. */
  std::vector<Transfer> clear_transfers_[2];

  /** Temporary storage for PerformTransfersAndResolveClears. */
  std::vector<TransferInvocation> current_transfer_invocations_;

  /** Temporary storage for DumpRenderTargets. */
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;
  std::vector<ResolveCopyDispatch> direct_resolve_dispatches_;
  u64 direct_resolve_attempt_count_ = 0;
  u64 direct_resolve_success_count_ = 0;
  u64 direct_resolve_fallback_count_ = 0;

  VkRenderPass fsi_render_pass_ = VK_NULL_HANDLE;
  Framebuffer fsi_framebuffer_;

  VkPipelineLayout resolve_fsi_clear_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_32bpp_pipeline_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_64bpp_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rex::gpu::vulkan
