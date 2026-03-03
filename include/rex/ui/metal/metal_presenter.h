/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 */

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <rex/ui/presenter.h>

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
@protocol MTLTexture;
@protocol MTLRenderPipelineState;
@protocol MTLRenderCommandEncoder;
@protocol MTLSamplerState;
@class CAMetalLayer;
#else
typedef void* id;
#endif

namespace rex {
namespace ui {
namespace metal {

class MetalProvider;

class MetalUIDrawContext final : public UIDrawContext {
 public:
  MetalUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height,
#ifdef __OBJC__
                     id<MTLRenderCommandEncoder> encoder,
#else
                     void* encoder,
#endif
                     uint64_t submission_index_current,
                     uint64_t submission_index_completed)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        encoder_(encoder),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed) {}

#ifdef __OBJC__
  id<MTLRenderCommandEncoder> encoder() const { return encoder_; }
#else
  void* encoder() const { return encoder_; }
#endif
  uint64_t submission_index_current() const {
    return submission_index_current_;
  }
  uint64_t submission_index_completed() const {
    return submission_index_completed_;
  }

 private:
#ifdef __OBJC__
  id<MTLRenderCommandEncoder> encoder_;
#else
  void* encoder_;
#endif
  uint64_t submission_index_current_;
  uint64_t submission_index_completed_;
};

class MetalPresenter : public Presenter {
 public:
  static std::unique_ptr<MetalPresenter> Create(
      MetalProvider& provider,
      HostGpuLossCallback host_gpu_loss_callback =
          FatalErrorHostGpuLossCallback);

  ~MetalPresenter() override;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width,
      uint32_t new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width,
      uint32_t frontbuffer_height,
      std::function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 public:
  class MetalGuestOutputRefreshContext : public GuestOutputRefreshContext {
   public:
#ifdef __OBJC__
    MetalGuestOutputRefreshContext(id<MTLTexture> texture, bool& is_8bpc_out)
        : GuestOutputRefreshContext(is_8bpc_out), texture_(texture) {}
    id<MTLTexture> texture() const { return texture_; }
#else
    MetalGuestOutputRefreshContext(void* texture, bool& is_8bpc_out)
        : GuestOutputRefreshContext(is_8bpc_out), texture_(texture) {}
    void* texture() const { return texture_; }
#endif
   private:
#ifdef __OBJC__
    id<MTLTexture> texture_;
#else
    void* texture_;
#endif
  };

 private:
  explicit MetalPresenter(MetalProvider& provider,
                          HostGpuLossCallback host_gpu_loss_callback);

  bool Initialize();

  // Creates the guest output textures for the mailbox.
  bool CreateGuestOutputTextures(uint32_t width, uint32_t height);
  void DestroyGuestOutputTextures();

  // Creates the render pipeline for guest output presentation.
  bool CreateGuestOutputPipeline();
  void DestroyGuestOutputPipeline();

  MetalProvider& provider_;

#ifdef __OBJC__
  CAMetalLayer* metal_layer_ = nil;

  // Guest output mailbox textures.
  std::array<id<MTLTexture>, kGuestOutputMailboxSize> guest_output_textures_{};
  uint32_t guest_output_texture_width_ = 0;
  uint32_t guest_output_texture_height_ = 0;

  // Pipeline for presenting the guest output to the surface.
  id<MTLRenderPipelineState> guest_output_pipeline_ = nil;
  id<MTLSamplerState> guest_output_sampler_bilinear_ = nil;
  id<MTLSamplerState> guest_output_sampler_nearest_ = nil;
#else
  void* metal_layer_ = nullptr;
  std::array<void*, kGuestOutputMailboxSize> guest_output_textures_{};
  uint32_t guest_output_texture_width_ = 0;
  uint32_t guest_output_texture_height_ = 0;
  void* guest_output_pipeline_ = nullptr;
  void* guest_output_sampler_bilinear_ = nullptr;
  void* guest_output_sampler_nearest_ = nullptr;
#endif

  // Cached staging texture for CaptureGuestOutput to avoid per-call allocation.
#ifdef __OBJC__
  id<MTLTexture> capture_staging_texture_ = nil;
#else
  void* capture_staging_texture_ = nullptr;
#endif
  uint32_t capture_staging_width_ = 0;
  uint32_t capture_staging_height_ = 0;

  // Presentation submission tracking for UI draw context.
  uint64_t present_submission_current_ = 1;
  std::atomic<uint64_t> present_submission_completed_{0};
};

}  // namespace metal
}  // namespace ui
}  // namespace rex
