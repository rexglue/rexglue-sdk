/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_METAL_METAL_IMMEDIATE_DRAWER_H_
#define XENIA_UI_METAL_METAL_IMMEDIATE_DRAWER_H_

#include <cstdint>
#include <memory>
#include <span>

#include "gpu/xenos/host/immediate_drawer.h"

namespace rex {
namespace ui {
namespace metal {

// Metal immediate-mode drawer for ImGui overlays: emulator dialogs (console /
// controller / host settings, profiles, achievements) AND guest system dialogs
// raised through XAM.
//
// Drawing these is not cosmetic. xeXamDispatchDialog blocks the calling guest
// thread on fence.Wait() until an ImGui-drawn modal is dismissed, so while this
// drawer did nothing, any title raising a system dialog hung there forever.
//
// Draws are recorded into the render encoder the presenter has already opened
// on the CAMetalLayer drawable (see MetalUIDrawContext), after the guest-output
// blit and before endEncoding. Nothing reachable from here touches EDRAM, guest
// render targets, or guest memory, so overlay drawing cannot perturb guest
// output.
//
// Metal objects are held as opaque void* so this header stays plain C++,
// matching MetalPresenter/MetalProvider. Owned handles are retained with
// CFBridgingRetain and released with CFBridgingRelease (this target builds with
// ARC), so the header needs no Objective-C.
class MetalImmediateDrawer : public ::rex::gpu::OverlayDrawer {
 public:
  MetalImmediateDrawer() = default;
  ~MetalImmediateDrawer() override;

  // device is an id<MTLDevice> owned by the MetalProvider. Builds the overlay
  // pipeline, the 1x1 white fallback texture, and the default sampler. Returns
  // false if any of those fail, in which case the drawer must not be used.
  bool Initialize(void* device);
  void Shutdown();

  [[nodiscard]] ::rex::Result<std::unique_ptr<::rex::gpu::OverlayTexture>> CreateTexture(
      uint32_t width, uint32_t height, ::rex::gpu::OverlayTextureFilter filter,
      bool is_repeated, std::span<const uint8_t> rgba8) override;

  // OnLeavePresenter is deliberately NOT overridden, unlike D3D12/Vulkan. They
  // use it to await GPU completion when leaving the presenter's submission
  // timeline, because they hand-track a per-texture last-usage submission index
  // and must not free a resource the GPU still reads. This drawer has no such
  // window: Metal itself retains every resource an encoder references until the
  // owning command buffer completes, and textures are held under ARC for their
  // own lifetime. Recorded in tools/metal_silent_gap_audit.sh so the absence
  // stays a decision rather than becoming a surprise.
  void Begin(::rex::gpu::OverlayDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginBatch(const ::rex::gpu::OverlayBatch& batch) override;
  void Draw(const ::rex::gpu::OverlayDraw& draw) override;
  void EndBatch() override;
  void End() override;

 private:
  // id<MTLTexture> + id<MTLSamplerState>, both retained for the texture's
  // lifetime and released in the destructor.
  class MetalImmediateTexture : public ::rex::gpu::OverlayTexture {
   public:
    MetalImmediateTexture(uint32_t width, uint32_t height)
        : ::rex::gpu::OverlayTexture(width, height) {}
    ~MetalImmediateTexture() override;

    void* texture = nullptr;
    void* sampler = nullptr;
  };

  // Not owned; the provider outlives the drawer.
  void* device_ = nullptr;

  // Owned.
  void* pipeline_ = nullptr;
  void* white_texture_ = nullptr;
  void* default_sampler_ = nullptr;

  // Valid only between Begin and End; not owned.
  void* current_render_encoder_ = nullptr;

  // Owned for the duration of one batch. The command buffer separately retains
  // any resource its encoders reference until completion, so releasing these at
  // EndBatch is safe even though the GPU has not finished with them.
  void* current_vertex_buffer_ = nullptr;
  void* current_index_buffer_ = nullptr;

  bool batch_open_ = false;
};

}  // namespace metal
}  // namespace ui
}  // namespace rex

#endif  // XENIA_UI_METAL_METAL_IMMEDIATE_DRAWER_H_
