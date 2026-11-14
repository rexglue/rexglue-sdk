/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/assert.h>
#include <rex/math.h>
#include "gpu/xenos/host/fixed_point.h"
#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/presenter.h"

namespace rex {
namespace gpu {

void OverlayDrawer::SetPresenter(Presenter* new_presenter) {
  if (presenter_ == new_presenter) {
    return;
  }
  REX_ASSERT(ui_draw_context_ == nullptr);
  if (presenter_) {
    OnLeavePresenter();
  }
  presenter_ = new_presenter;
  if (presenter_) {
    OnEnterPresenter();
  }
}

void OverlayDrawer::Begin(gpu::OverlayDrawContext& context, f32 coordinate_space_width,
                          f32 coordinate_space_height) {
  REX_ASSERT(context.GetPresenter() == presenter_);
  ui_draw_context_ = &context;
  if (!(coordinate_space_width > 0.0f) || !(coordinate_space_height > 0.0f)) {
    coordinate_space_width = static_cast<f32>(context.GetRenderTargetWidth());
    coordinate_space_height = static_cast<f32>(context.GetRenderTargetHeight());
  }
  coordinate_space_width_ = coordinate_space_width;
  coordinate_space_height_ = coordinate_space_height;
}

void OverlayDrawer::End() {
  ui_draw_context_ = nullptr;
}

bool OverlayDrawer::ScissorToRenderTarget(const gpu::OverlayDraw& draw, u32& out_left, u32& out_top,
                                          u32& out_width, u32& out_height) {
  u32 render_target_width = GetOverlayDrawContext()->GetRenderTargetWidth();
  u32 render_target_height = GetOverlayDrawContext()->GetRenderTargetHeight();
  if (!draw.scissor) {
    out_left = 0;
    out_top = 0;
    out_width = render_target_width;
    out_height = render_target_height;
    return render_target_width && render_target_height;
  }
  const gpu::OverlayRect& scissor = *draw.scissor;
  f32 render_target_width_float = static_cast<f32>(render_target_width);
  f32 render_target_height_float = static_cast<f32>(render_target_height);
  f32 scale_x = render_target_width_float / GetCoordinateSpaceWidth();
  f32 scale_y = render_target_height_float / GetCoordinateSpaceHeight();
  f32 x0_float = rex::clamp_float(scissor.left * scale_x, 0.0f, render_target_width_float);
  f32 y0_float = rex::clamp_float(scissor.top * scale_y, 0.0f, render_target_height_float);
  f32 x1_float = rex::clamp_float(scissor.right * scale_x, x0_float, render_target_width_float);
  f32 y1_float = rex::clamp_float(scissor.bottom * scale_y, y0_float, render_target_height_float);
  i32 x0 = (FloatToD3D11Fixed16p8(x0_float) + 127) >> 8;
  i32 y0 = (FloatToD3D11Fixed16p8(y0_float) + 127) >> 8;
  i32 x1 = (FloatToD3D11Fixed16p8(x1_float) + 127) >> 8;
  i32 y1 = (FloatToD3D11Fixed16p8(y1_float) + 127) >> 8;
  REX_ASSERT(x0 >= 0);
  REX_ASSERT(y0 >= 0);
  REX_ASSERT(x1 >= x0);
  REX_ASSERT(y1 >= y0);
  REX_ASSERT(x1 <= static_cast<i32>(render_target_width));
  REX_ASSERT(y1 <= static_cast<i32>(render_target_height));
  out_left = static_cast<u32>(x0);
  out_top = static_cast<u32>(y0);
  out_width = static_cast<u32>(x1 - x0);
  out_height = static_cast<u32>(y1 - y0);
  return x1 > x0 && y1 > y0;
}

}  // namespace gpu
}  // namespace rex
