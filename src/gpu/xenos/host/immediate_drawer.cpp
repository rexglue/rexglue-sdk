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
#include "gpu/xenos/host/graphics_util.h"
#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/presenter.h"

namespace rex {
namespace gpu {

void ImmediateDrawer::SetPresenter(Presenter* new_presenter) {
  if (presenter_ == new_presenter) {
    return;
  }
  // Changing the presenter while drawing would make the state inconsistent.
  assert_null(ui_draw_context_);
  if (presenter_) {
    OnLeavePresenter();
  }
  presenter_ = new_presenter;
  if (presenter_) {
    OnEnterPresenter();
  }
}

void ImmediateDrawer::Begin(gpu::OverlayDrawContext& context, f32 coordinate_space_width,
                            f32 coordinate_space_height) {
  assert_true(context.presenter() == presenter_);
  ui_draw_context_ = &context;
  // In case of non-positive values (or NaNs) - use render target coordinates
  // according to the contract of the function, and also for safety because
  // there will be division by the coordinate space size in several places.
  if (!(coordinate_space_width > 0.0f) || !(coordinate_space_height > 0.0f)) {
    coordinate_space_width = f32(context.render_target_width());
    coordinate_space_height = f32(context.render_target_height());
  }
  coordinate_space_width_ = coordinate_space_width;
  coordinate_space_height_ = coordinate_space_height;
}

void ImmediateDrawer::End() {
  ui_draw_context_ = nullptr;
}

bool ImmediateDrawer::ScissorToRenderTarget(const gpu::OverlayDraw& draw, u32& out_left,
                                            u32& out_top, u32& out_width, u32& out_height) {
  u32 render_target_width = ui_draw_context()->render_target_width();
  u32 render_target_height = ui_draw_context()->render_target_height();
  if (!draw.scissor) {
    out_left = 0;
    out_top = 0;
    out_width = render_target_width;
    out_height = render_target_height;
    return render_target_width && render_target_height;
  }
  const gpu::OverlayRect& scissor = *draw.scissor;
  float render_target_width_float = float(render_target_width);
  float render_target_height_float = float(render_target_height);
  // Scale to render target coordinates, drop NaNs, and clamp to the render
  // target size, below which the values are representable as 16p8 fixed-point.
  float scale_x = render_target_width / coordinate_space_width();
  float scale_y = render_target_height / coordinate_space_height();
  float x0_float = rex::clamp_float(scissor.left * scale_x, 0.0f, render_target_width_float);
  float y0_float = rex::clamp_float(scissor.top * scale_y, 0.0f, render_target_height_float);
  // Also make sure the size is non-negative.
  float x1_float = rex::clamp_float(scissor.right * scale_x, x0_float, render_target_width_float);
  float y1_float = rex::clamp_float(scissor.bottom * scale_y, y0_float, render_target_height_float);
  // Top-left - include .5 (0.128 treated as 0 covered, 0.129 as 0 not covered).
  int32_t x0 = (FloatToD3D11Fixed16p8(x0_float) + 127) >> 8;
  int32_t y0 = (FloatToD3D11Fixed16p8(y0_float) + 127) >> 8;
  // Bottom-right - exclude .5.
  int32_t x1 = (FloatToD3D11Fixed16p8(x1_float) + 127) >> 8;
  int32_t y1 = (FloatToD3D11Fixed16p8(y1_float) + 127) >> 8;
  assert_true(x0 >= 0);
  assert_true(y0 >= 0);
  assert_true(x1 >= x0);
  assert_true(y1 >= y0);
  assert_true(x1 <= int32_t(render_target_width));
  assert_true(y1 <= int32_t(render_target_height));
  out_left = uint32_t(x0);
  out_top = uint32_t(y0);
  out_width = uint32_t(x1 - x0);
  out_height = uint32_t(y1 - y0);
  return x1 > x0 && y1 > y0;
}

}  // namespace gpu
}  // namespace rex
