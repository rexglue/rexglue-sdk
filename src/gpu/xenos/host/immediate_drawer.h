#pragma once
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

#include <memory>

#include <rex/gpu/overlay.h>
#include "gpu/xenos/host/presenter.h"

namespace rex {
namespace gpu {

class ImmediateDrawer : public gpu::IOverlayDrawer {
 public:
  ImmediateDrawer(const ImmediateDrawer& immediate_drawer) = delete;
  ImmediateDrawer& operator=(const ImmediateDrawer& immediate_drawer) = delete;

  virtual ~ImmediateDrawer() = default;

  void SetPresenter(Presenter* new_presenter);

  void Begin(gpu::OverlayDrawContext& context, f32 width, f32 height) override;
  void End() override;

 protected:
  ImmediateDrawer() = default;

  Presenter* presenter() const { return presenter_; }
  virtual void OnLeavePresenter() {}
  virtual void OnEnterPresenter() {}

  // Available between Begin and End.
  gpu::OverlayDrawContext* ui_draw_context() const { return ui_draw_context_; }
  f32 coordinate_space_width() const { return coordinate_space_width_; }
  f32 coordinate_space_height() const { return coordinate_space_height_; }

  // Converts and clamps the scissor in the immediate draw to render target
  // coordinates. Returns whether the scissor contains any render target pixels
  // (but a valid scissor is written even if false is returned).
  bool ScissorToRenderTarget(const gpu::OverlayDraw& draw, u32& out_left, u32& out_top,
                             u32& out_width, u32& out_height);

 private:
  Presenter* presenter_ = nullptr;

  gpu::OverlayDrawContext* ui_draw_context_ = nullptr;
  f32 coordinate_space_width_;
  f32 coordinate_space_height_;
};

}  // namespace gpu
}  // namespace rex
