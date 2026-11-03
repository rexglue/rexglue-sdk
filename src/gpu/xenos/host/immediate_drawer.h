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

#include <memory>

#include "gpu/xenos/host/overlay.h"
#include <rex/types.h>
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/xenos_export.h"

namespace rex {
namespace gpu {

class REX_GPU_XENOS_API OverlayDrawer : public gpu::IOverlayDrawer {
 public:
  virtual ~OverlayDrawer() = default;

  void SetPresenter(Presenter* new_presenter);

  void Begin(gpu::OverlayDrawContext& context, f32 width, f32 height) override;
  void End() override;

 protected:
  OverlayDrawer() = default;

  Presenter* GetPresenter() const { return presenter_; }
  virtual void OnLeavePresenter() {}
  virtual void OnEnterPresenter() {}

  /** Returns the context, valid between Begin and End. */
  gpu::OverlayDrawContext* GetOverlayDrawContext() const { return ui_draw_context_; }
  f32 GetCoordinateSpaceWidth() const { return coordinate_space_width_; }
  f32 GetCoordinateSpaceHeight() const { return coordinate_space_height_; }

  /**
   * Converts the draw scissor to render target pixels, clamped to the render target size.
   *
   * A valid scissor is written even when the result is false. The top left edge includes the 0.5
   * sample and the bottom right edge excludes it.
   *
   * @param draw Draw holding the scissor.
   * @param out_left Receives the left edge.
   * @param out_top Receives the top edge.
   * @param out_width Receives the width.
   * @param out_height Receives the height.
   * @return True when the scissor contains any render target pixels.
   */
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
