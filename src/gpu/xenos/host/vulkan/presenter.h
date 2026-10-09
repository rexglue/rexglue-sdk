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
#include <rex/gpu/surface.h>
#include <rex/types.h>
#include "gpu/xenos/host/vulkan/device.h"
#include "gpu/xenos/host/vulkan/instance.h"
#include "gpu/xenos/host/vulkan/submission_tracker.h"
#include "gpu/xenos/host/vulkan/ui_samplers.h"

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

  VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }
  u64 submission_index_current() const { return submission_index_current_; }
  u64 submission_index_completed() const { return submission_index_completed_; }
  VkRenderPass render_pass() const { return render_pass_; }
  VkFormat render_pass_format() const { return render_pass_format_; }

 private:
  VkCommandBuffer draw_command_buffer_;
  u64 submission_index_current_;
  u64 submission_index_completed_;
  /** Has 1 subpass with a single render_pass_format_ attachment. */
  VkRenderPass render_pass_;
  VkFormat render_pass_format_;
};

class VulkanPresenter final : public Presenter {
  struct Key {};

 public:
  explicit VulkanPresenter(Key, HostGpuLossCallback host_gpu_loss_callback,
                           const VulkanDevice* vulkan_device, const UISamplers* ui_samplers)
      : Presenter(std::move(host_gpu_loss_callback)),
        vulkan_device_(vulkan_device),
        ui_samplers_(ui_samplers),
        guest_output_image_refresher_submission_tracker_(vulkan_device),
        ui_submission_tracker_(vulkan_device),
        paint_context_(vulkan_device) {
    REX_ASSERT(vulkan_device != nullptr);
    REX_ASSERT(ui_samplers != nullptr);
  }

  /**
   * Maximum number of guest output image versions that may still be alive and given to the
   * refresher.
   *
   * The refresher may need to keep this many instances of dependent objects such as framebuffers
   * across invocations, as multiple buffering in the presenter can give a different version on
   * every invocation, to avoid recreating them every frame.
   */
  static constexpr size_t kMaxActiveGuestOutputImageVersions = kGuestOutputMailboxSize;

  static constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  /**
   * Stage, access and layout in which the refresher acquires and releases the guest output.
   *
   * The first write to the current guest output image only needs a barrier from
   * VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT without access. An image refreshed for the first time is
   * in VK_IMAGE_LAYOUT_UNDEFINED, and transitioning from it when writing is safe and preferred.
   */
  static constexpr VkPipelineStageFlagBits kGuestOutputInternalStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  static constexpr VkAccessFlags kGuestOutputInternalAccessMask = VK_ACCESS_SHADER_READ_BIT;
  static constexpr VkImageLayout kGuestOutputInternalLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  /** The refresher callback must use the graphics and compute queue 0 of the device. */
  class VulkanGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    VulkanGuestOutputRefreshContext(bool& is_8bpc_out_ref, VkImage image, VkImageView image_view,
                                    u64 image_version, bool image_ever_written_previously)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          image_(image),
          image_view_(image_view),
          image_version_(image_version),
          image_ever_written_previously_(image_ever_written_previously) {}

    /**
     * Returns the guest output image.
     *
     * The format is kGuestOutputFormat. Usable as a color attachment, a sampled image and a
     * transfer source, the last for guest output capturing.
     *
     * @return The image.
     */
    VkImage image() const { return image_; }
    VkImageView image_view() const { return image_view_; }
    u64 image_version() const { return image_version_; }
    /**
     * Reports whether a full barrier must be done to acquire the image.
     *
     * @return True when the image was written before.
     */
    bool image_ever_written_previously() const { return image_ever_written_previously_; }

   private:
    VkImage image_;
    VkImageView image_view_;
    u64 image_version_;
    bool image_ever_written_previously_;
  };

  static std::unique_ptr<VulkanPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 const VulkanDevice* vulkan_device,
                                                 const UISamplers* ui_samplers) {
    auto presenter = std::make_unique<VulkanPresenter>(Key{}, std::move(host_gpu_loss_callback),
                                                       vulkan_device, ui_samplers);
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  /**
   * Destroys the swapchain once its images are no longer used for drawing, then awaits usage of
   * everything before destroying anything.
   *
   * vkQueuePresentKHR signals no fence showing when destroying a swapchain is safe, so its
   * lifetime is assumed tracked inside the WSI. The swapchain is destroyed before the semaphore
   * awaited by vkQueuePresentKHR, hoping to avoid destruction during that wait or between the
   * vkQueueSubmit signal and the present wait. This also awaits all paint submissions. Trackers
   * are shut down starting with the UI submission tracker, then the guest output image
   * refresher.
   */
  ~VulkanPresenter();

  const VulkanDevice* vulkan_device() const { return vulkan_device_; }

  static gpu::Surface::TypeFlags GetSurfaceTypesSupportedByInstance(
      const VulkanInstance::Extensions& instance_extensions);
  gpu::Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  void AwaitUISubmissionCompletionFromUIThread(u64 submission_index) {
    ui_submission_tracker_.AwaitSubmissionCompletion(submission_index);
  }
  VkCommandBuffer AcquireUISetupCommandBufferFromUIThread();

 protected:
  /**
   * Connects painting to a surface, or reconnects it.
   *
   * If the gpu::Surface is the same, first tries to create the swapchain from the existing
   * VkSurface and swapchain, and starts from scratch when that fails. Retiring or destroying the
   * swapchain awaits GPU usage of the swapchain and the surface.
   *
   * @param new_surface The surface to paint to.
   * @param new_surface_width The surface width.
   * @param new_surface_height The surface height.
   * @param was_paintable True when painting was connected before.
   * @param is_vsync_implicit_out Receives whether vsync is implicit.
   * @return The connection result.
   */
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      gpu::Surface& new_surface, u32 new_surface_width, u32 new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(
      u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
      std::move_only_function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

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

    const VkExtent2D& extent() const { return extent_; }

    VkImage image() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }
    VkImageView view() const { return view_; }

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
    /**
     * Selects the barrier stage, access mask and layout.
     *
     * True for any image active after a refresh.
     */
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

  enum GuestOutputPaintPipelineLayoutIndex : size_t {
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

      VkSemaphore acquire_semaphore() const { return acquire_semaphore_; }
      VkSemaphore present_semaphore() const { return present_semaphore_; }
      VkCommandPool draw_command_pool() const { return draw_command_pool_; }
      VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }

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
      /**
       * Created during guest output painting, after awaiting the last paint when outdated, for
       * the up to date swapchain render pass with the swapchain image format.
       */
      VkPipeline swapchain_pipeline = VK_NULL_HANDLE;
      VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    };

    enum GuestOutputDescriptorSet : u32 {
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
     * The old swapchain, if passed, is treated as retired after this call. It may be in an
     * undefined state if the call fails before vkCreateSwapchainKHR retires it, and the caller
     * must destroy it whatever the result.
     *
     * A zero area surface returns null without setting ui_surface_unusable_out, as the other
     * fields may carry no information and the flag may be unrecoverable on some platforms. This
     * happens for VkSurfaceKHR bounds in some Windows window states. The requested size is
     * clamped to the physical device maximum while minImageExtent of the instance WSI allows it,
     * otherwise creation fails until the surface shrinks.
     *
     * When the graphics and compute queue family cannot present, the concurrent sharing mode
     * avoids an ownership transfer before presenting, which would need an extra command buffer
     * submission with the acquire barrier and a semaphore between the queues. Different queues
     * are rare and the swapchain only carries the final guest output and the internal UI,
     * so keeping framebuffer compression is not worth the extra submission complexity and latency.
     *
     * Present mode prefers modes that allow quick acquisition, so GPU command processing is not
     * disturbed, and tearing for variable refresh rate. IMMEDIATE tears to cut latency, though
     * Windows borderless fullscreen uses GDI copying instead of independent flip and does not
     * support it. MAILBOX drops frames to cut latency without tearing. FIFO_RELAXED limits the
     * frame rate but lets overlong frames tear. FIFO has the highest latency and is always
     * available.
     *
     * @param vulkan_device The device.
     * @param surface The surface.
     * @param width The requested width.
     * @param height The requested height.
     * @param old_swapchain The swapchain to replace, or null.
     * @param present_queue_family_out Receives the presentation queue family.
     * @param image_format_out Receives the swapchain image format.
     * @param image_extent_out Receives the swapchain image extent.
     * @param is_fifo_out Receives whether the present mode is FIFO.
     * @param ui_surface_unusable_out Receives whether the surface is unusable for the UI.
     * @return The swapchain, or null on failure.
     */
    static VkSwapchainKHR CreateSwapchainForVulkanSurface(
        const VulkanDevice* vulkan_device, VkSurfaceKHR surface, u32 width, u32 height,
        VkSwapchainKHR old_swapchain, u32& present_queue_family_out, VkFormat& image_format_out,
        VkExtent2D& image_extent_out, bool& is_fifo_out, bool& ui_surface_unusable_out);

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
     * Refreshed and cleaned up during guest output painting.
     *
     * Each pair holds the paint submission index that last used the guest output image and its
     * descriptors, and the image reference, which may be null. The indices are not mailbox
     * indices. A reference missing from the array replaces the most outdated one, after awaiting
     * the usage completion of its last paint.
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

  /**
   * Creates the objects that do not depend on a surface connection.
   *
   * The descriptor pool needs a sampler descriptor type even with immutable samplers, otherwise
   * descriptor set allocation fails.
   *
   * @return True on success.
   */
  bool InitializeSurfaceIndependent();

  [[nodiscard]] VkPipeline CreateGuestOutputPaintPipeline(GuestOutputPaintEffect effect,
                                                          VkRenderPass render_pass);

  const VulkanDevice* vulkan_device_;
  const UISamplers* ui_samplers_;

  /**
   * Static objects for guest output presentation, used only when painting the main target.
   *
   * Destroy only after awaiting main target usage completion.
   */
  VkDescriptorSetLayout guest_output_paint_image_descriptor_set_layout_ = VK_NULL_HANDLE;
  std::array<VkPipelineLayout, kGuestOutputPaintPipelineLayoutCount>
      guest_output_paint_pipeline_layouts_ = {};
  VkShaderModule guest_output_paint_vs_ = VK_NULL_HANDLE;
  std::array<VkShaderModule, static_cast<size_t>(GuestOutputPaintEffect::kCount)>
      guest_output_paint_fs_ = {};

  /**
   * Increased monotonically whenever a new guest output image is initialized.
   *
   * Refreshers use it to recreate dependent objects such as framebuffers, since comparing handles
   * is not enough when Create, Destroy, Create yields the same handle for different objects.
   */
  u64 guest_output_image_next_version_ = 0;
  std::array<GuestOutputImageInstance, kGuestOutputMailboxSize> guest_output_images_;
  VulkanSubmissionTracker guest_output_image_refresher_submission_tracker_;

  /** UI submission tracker whose submission index may be given to UI drawers. UI thread only. */
  VulkanSubmissionTracker ui_submission_tracker_;

  /**
   * Accessible only by painting, by the thread running surface connection lifetime management
   * (ConnectOrReconnectPaintingToSurfaceFromUIThread and
   * DisconnectPaintingFromSurfaceFromUIThreadImpl), and by presenter initialization and shutdown.
   */
  PaintContext paint_context_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
