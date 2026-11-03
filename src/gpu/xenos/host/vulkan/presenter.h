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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/host/surface.h"
#include <rex/types.h>
#include <rex/utility.h>
#include "gpu/xenos/host/vulkan/device.h"
#include "gpu/xenos/host/vulkan/instance.h"
#include "gpu/xenos/host/vulkan/submission_tracker.h"
#include "gpu/xenos/host/vulkan/ui_sampler.h"

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanOverlayDrawContext final : public gpu::OverlayDrawContext {
 public:
  VulkanOverlayDrawContext(Presenter& presenter, u32 render_target_width, u32 render_target_height,
                           VkCommandBuffer draw_command_buffer, u64 submission_index_current,
                           u64 submission_index_completed, VkRenderPass render_pass,
                           VkFormat render_pass_format)
      : gpu::OverlayDrawContext(&presenter, render_target_width, render_target_height),
        draw_command_buffer_(draw_command_buffer),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed),
        render_pass_(render_pass),
        render_pass_format_(render_pass_format) {}

  VkCommandBuffer GetDrawCommandBuffer() const { return draw_command_buffer_; }
  u64 GetSubmissionIndexCurrent() const { return submission_index_current_; }
  u64 GetSubmissionIndexCompleted() const { return submission_index_completed_; }
  VkRenderPass GetRenderPass() const { return render_pass_; }
  VkFormat GetRenderPassFormat() const { return render_pass_format_; }

 private:
  VkCommandBuffer draw_command_buffer_;
  u64 submission_index_current_;
  u64 submission_index_completed_;
  /** Has one subpass with one render_pass_format_ attachment. */
  VkRenderPass render_pass_;
  VkFormat render_pass_format_;
};

class VulkanPresenter final : public Presenter {
  struct Key {};

 public:
  explicit VulkanPresenter(Key, HostGPULossCallback host_gpu_loss_callback,
                           const XenosGraphicsDriverBase& driver, const VulkanSettings& settings,
                           const VulkanDevice* vulkan_device, const UISamplerSet* ui_samplers)
      : Presenter(std::move(host_gpu_loss_callback), driver),
        settings_(settings),
        vulkan_device_(vulkan_device),
        ui_samplers_(ui_samplers),
        guest_output_image_refresher_submission_tracker_(vulkan_device),
        ui_submission_tracker_(vulkan_device),
        paint_context_(vulkan_device) {
    REX_ASSERT(vulkan_device != nullptr);
    REX_ASSERT(ui_samplers != nullptr);
  }

  /** Maximum number of guest output image versions alive at once. */
  static constexpr size_t kMaxActiveGuestOutputImageVersions = kGuestOutputMailboxSize;

  static constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  /** Stage, access and layout in which the refresher acquires and releases the guest output. */
  static constexpr VkPipelineStageFlagBits kGuestOutputInternalStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  static constexpr VkAccessFlags kGuestOutputInternalAccessMask = VK_ACCESS_SHADER_READ_BIT;
  static constexpr VkImageLayout kGuestOutputInternalLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  /** The refresher callback must use the graphics and compute queue 0 of the device. */
  class VulkanGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    VulkanGuestOutputRefreshContext(bool& is_8bpc_out, VkImage image, VkImageView image_view,
                                    u64 image_version, bool image_ever_written_previously)
        : GuestOutputRefreshContext(is_8bpc_out),
          image_(image),
          image_view_(image_view),
          image_version_(image_version),
          image_written_(image_ever_written_previously) {}

    /**
     * Returns the guest output image in kGuestOutputFormat, usable as color attachment, sampled
     * image and transfer source.
     */
    VkImage GetImage() const { return image_; }
    VkImageView GetImageView() const { return image_view_; }
    u64 GetImageVersion() const { return image_version_; }
    /** Reports whether acquiring the image needs a full barrier. */
    bool HasImageBeenWritten() const { return image_written_; }

   private:
    VkImage image_;
    VkImageView image_view_;
    u64 image_version_;
    bool image_written_;
  };

  static std::unique_ptr<VulkanPresenter> Create(HostGPULossCallback host_gpu_loss_callback,
                                                 const XenosGraphicsDriverBase& driver,
                                                 const VulkanSettings& settings,
                                                 const VulkanDevice* vulkan_device,
                                                 const UISamplerSet* ui_samplers) {
    auto presenter = std::make_unique<VulkanPresenter>(
        Key{}, std::move(host_gpu_loss_callback), driver, settings, vulkan_device, ui_samplers);
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  /**
   * Awaits all paint submissions, then destroys the swapchain before the semaphore that
   * vkQueuePresentKHR waits on.
   */
  ~VulkanPresenter();

  const VulkanDevice* GetVulkanDevice() const { return vulkan_device_; }

  static gpu::Surface::TypeFlags GetSurfaceTypesSupportedByInstance(
      const VulkanInstance::Extensions& instance_extensions);
  gpu::Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  void AwaitUISubmissionCompletionFromUIThread(u64 submission_index) {
    ui_submission_tracker_.AwaitSubmissionCompletion(submission_index);
  }
  VkCommandBuffer AcquireUISetupCommandBufferFromUIThread();

 protected:
  /**
   * @param new_surface_width The width of new_surface in pixels.
   * @param new_surface_height The height of new_surface in pixels.
   * @param was_paintable True when painting was connected before.
   * @param is_vsync_implicit_out Receives whether vsync is implicit.
   * @return The connection result.
   */
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      gpu::Surface& new_surface, u32 new_surface_width, u32 new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DriverDisconnectPaintingFromSurface() override;

  bool DriverRefreshGuestOutput(
      u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
      rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out) override;

  PaintResult DriverPaintAndPresent(bool execute_ui_drawers) override;

 private:
  class GuestOutputImage : public rex::NonCopyable {
    struct Key {};

   public:
    GuestOutputImage(Key, const VulkanDevice* const vulkan_device, const u32 width,
                     const u32 height)
        : vulkan_device_(vulkan_device) {
      extent_.width = width;
      extent_.height = height;
    }

    static std::unique_ptr<GuestOutputImage> Create(const VulkanDevice* const vulkan_device,
                                                    const u32 width, const u32 height) {
      REX_ASSERT(width != 0);
      REX_ASSERT(height != 0);
      auto image = std::make_unique<GuestOutputImage>(Key{}, vulkan_device, width, height);
      if (!image->Initialize()) {
        return nullptr;
      }
      return image;
    }

    ~GuestOutputImage();

    const VkExtent2D& GetExtent() const { return extent_; }

    VkImage GetImage() const { return image_; }
    VkDeviceMemory GetMemory() const { return memory_; }
    VkImageView GetView() const { return view_; }

   private:
    bool Initialize();

    const VulkanDevice* vulkan_device_;

    VkExtent2D extent_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
  };

  struct GuestOutputImageInstance {
    /** Refresher side reference. Painting holds its own so destruction waits for the GPU. */
    std::shared_ptr<GuestOutputImage> image;
    u64 version = UINT64_MAX;
    u64 last_refresher_submission = 0;
    /** True once the refresher has refreshed this image. */
    bool ever_successfully_refreshed = false;

    void SetToNewImage(const std::shared_ptr<GuestOutputImage>& new_image, u64 new_version) {
      image = new_image;
      version = new_version;
      last_refresher_submission = 0;
      ever_successfully_refreshed = false;
    }
  };

  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        f32 x;
        f32 y;
      };
      f32 offset[2];
    };
    union {
      struct {
        f32 width;
        f32 height;
      };
      f32 size[2];
    };
  };

  enum GuestOutputPaintPipelineLayoutIndex : u8 {
    kGuestOutputPaintPipelineLayoutIndexBilinear,

    kGuestOutputPaintPipelineLayoutCount,
  };

  static constexpr GuestOutputPaintPipelineLayoutIndex GetGuestOutputPaintPipelineLayoutIndex(
      GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintPipelineLayoutIndexBilinear;
      default:
        REX_UNHANDLED_CASE(effect);
        return kGuestOutputPaintPipelineLayoutCount;
    }
  }

  struct PaintContext : public rex::NonCopyable {
    class Submission : public rex::NonCopyable {
      struct Key {};

     public:
      explicit Submission(Key, const VulkanDevice* const vulkan_device)
          : vulkan_device_(vulkan_device) {}

      static std::unique_ptr<Submission> Create(const VulkanDevice* const vulkan_device) {
        auto submission = std::make_unique<Submission>(Key{}, vulkan_device);
        if (!submission->Initialize()) {
          return nullptr;
        }
        return submission;
      }

      ~Submission();

      VkSemaphore GetAcquireSemaphore() const { return acquire_semaphore_; }
      VkSemaphore GetPresentSemaphore() const { return present_semaphore_; }
      VkCommandPool GetDrawCommandPool() const { return draw_command_pool_; }
      VkCommandBuffer GetDrawCommandBuffer() const { return draw_command_buffer_; }

     private:
      bool Initialize();

      const VulkanDevice* vulkan_device_;
      VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
      VkSemaphore present_semaphore_ = VK_NULL_HANDLE;
      VkCommandPool draw_command_pool_ = VK_NULL_HANDLE;
      VkCommandBuffer draw_command_buffer_ = VK_NULL_HANDLE;
    };

    static constexpr u32 kSubmissionCount = 3;

    struct GuestOutputPaintPipeline {
      /** Created during guest output painting for the current swapchain render pass and format. */
      VkPipeline swapchain_pipeline = VK_NULL_HANDLE;
      VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    };

    enum GuestOutputDescriptorSet : u8 {
      kGuestOutputDescriptorSetGuestOutput0Sampled,

      kGuestOutputDescriptorSetCount =
          kGuestOutputDescriptorSetGuestOutput0Sampled + kGuestOutputMailboxSize,
    };

    struct UISetupCommandBuffer {
      UISetupCommandBuffer(VkCommandPool command_pool, VkCommandBuffer command_buffer,
                           u64 last_usage_submission_index = 0)
          : command_pool(command_pool),
            command_buffer(command_buffer),
            last_usage_submission_index(last_usage_submission_index) {}

      VkCommandPool command_pool;
      VkCommandBuffer command_buffer;
      u64 last_usage_submission_index;
    };

    struct SwapchainFramebuffer {
      SwapchainFramebuffer(VkImageView image_view, VkFramebuffer framebuffer)
          : image_view(image_view), framebuffer(framebuffer) {}

      VkImageView image_view;
      VkFramebuffer framebuffer;
    };

    explicit PaintContext(const VulkanDevice* const vulkan_device)
        : vulkan_device(vulkan_device), submission_tracker(vulkan_device) {}

    /**
     * Creates a swapchain for the surface.
     *
     * The old swapchain is treated as retired after this call and the caller must destroy it
     * whatever the result.
     *
     * A zero area surface returns null without setting ui_surface_unusable_out, as some Windows
     * window states report such bounds. The requested size is clamped to the physical device
     * maximum while minImageExtent allows it, otherwise creation fails until the surface shrinks.
     *
     * @param vulkan_device The device that owns the swapchain.
     * @param settings The gpu.vulkan settings.
     * @param surface The surface to create the swapchain for.
     * @param width The requested width.
     * @param height The requested height.
     * @param vsync True to synchronize presentation to the display refresh.
     * @param old_swapchain The swapchain to replace, or null.
     * @param present_queue_family_out Receives the presentation queue family.
     * @param image_format_out Receives the swapchain image format.
     * @param image_extent_out Receives the swapchain image extent.
     * @param is_fifo_out Receives whether the present mode is FIFO.
     * @param ui_surface_unusable_out Receives whether the surface is unusable for the UI.
     * @return The swapchain, or null on failure.
     */
    static VkSwapchainKHR CreateSwapchainForVulkanSurface(
        const VulkanDevice* vulkan_device, const VulkanSettings& settings, VkSurfaceKHR surface,
        u32 width, u32 height, bool vsync, VkSwapchainKHR old_swapchain,
        u32& present_queue_family_out, VkFormat& image_format_out, VkExtent2D& image_extent_out,
        bool& is_fifo_out, bool& ui_surface_unusable_out);

    /**
     * Destroys the swapchain derivatives and nulls swapchain.
     *
     * @return The original swapchain, if any, for use as oldSwapchain and for destruction.
     */
    VkSwapchainKHR PrepareForSwapchainRetirement();
    /** Destroys the swapchain and the surface. May be called from the presenter destructor. */
    void DestroySwapchainAndVulkanSurface();

    const VulkanDevice* vulkan_device;

    std::array<std::unique_ptr<PaintContext::Submission>, kSubmissionCount> submissions;
    VulkanSubmissionTracker submission_tracker;

    std::array<GuestOutputPaintPipeline, static_cast<size_t>(GuestOutputPaintEffect::kCount)>
        guest_output_paint_pipelines;

    VkDescriptorPool guest_output_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet guest_output_descriptor_sets[kGuestOutputDescriptorSetCount];

    /**
     * Pairs of the paint submission index that last used the image and the image reference,
     * which may be null. The indices are not mailbox indices.
     */
    std::array<std::pair<u64, std::shared_ptr<GuestOutputImage>>, kGuestOutputMailboxSize>
        guest_output_image_paint_refs;
    u64 guest_output_image_paint_last_submission = 0;

    /** Command buffers optionally run before the draw command buffer, outside the render pass. */
    std::vector<UISetupCommandBuffer> ui_setup_command_buffers;
    size_t ui_setup_command_buffer_current_index = SIZE_MAX;

    /** May be reused between connections while the format stays the same. */
    VkRenderPass swapchain_render_pass = VK_NULL_HANDLE;
    VkFormat swapchain_render_pass_format = VK_FORMAT_UNDEFINED;
    bool swapchain_render_pass_clear_load_op = false;

    VkSurfaceKHR vulkan_surface = VK_NULL_HANDLE;
    u32 present_queue_family = UINT32_MAX;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D swapchain_extent = {};
    bool swapchain_is_fifo = false;
    std::vector<VkImage> swapchain_images;
    std::vector<SwapchainFramebuffer> swapchain_framebuffers;
  };

  bool InitializeSurfaceIndependent();

  [[nodiscard]] VkPipeline CreateGuestOutputPaintPipeline(GuestOutputPaintEffect effect,
                                                          VkRenderPass render_pass);

  const VulkanSettings& settings_;
  const VulkanDevice* vulkan_device_;
  const UISamplerSet* ui_samplers_;

  /** Static guest output presentation objects. Destroy only after main target usage completes. */
  VkDescriptorSetLayout guest_output_paint_image_descriptor_set_layout_ = VK_NULL_HANDLE;
  std::array<VkPipelineLayout, kGuestOutputPaintPipelineLayoutCount>
      guest_output_paint_pipeline_layouts_ = {};
  VkShaderModule guest_output_paint_vs_ = VK_NULL_HANDLE;
  std::array<VkShaderModule, static_cast<size_t>(GuestOutputPaintEffect::kCount)>
      guest_output_paint_fs_ = {};

  /** Increases whenever a new guest output image is initialized, as a reused handle may differ. */
  u64 guest_output_image_next_version_ = 0;
  std::array<GuestOutputImageInstance, kGuestOutputMailboxSize> guest_output_images_;
  VulkanSubmissionTracker guest_output_image_refresher_submission_tracker_;

  /** UI submission tracker whose submission index may be given to UI drawers. UI thread only. */
  VulkanSubmissionTracker ui_submission_tracker_;

  /** Touched only by painting, surface connect and disconnect, and presenter init and shutdown. */
  PaintContext paint_context_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
