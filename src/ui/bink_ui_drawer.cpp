#include "rex/ui/bink_ui_drawer.h"

#include <algorithm>
#include <cstring>

#include "rex/system/bink_overlay.h"
#include "rex/ui/immediate_drawer.h"
#include "rex/ui/presenter.h"

namespace rex::ui {

namespace {

constexpr size_t kBinkUIDrawerZOrder = 1;  // Below ImGuiDrawer (defaults higher).

}  // namespace

BinkUIDrawer::BinkUIDrawer() = default;

BinkUIDrawer::~BinkUIDrawer() {
  SetPresenter(nullptr, nullptr);
}

void BinkUIDrawer::SetPresenter(Presenter* presenter,
                                ImmediateDrawer* immediate_drawer) {
  if (presenter == presenter_ && immediate_drawer == immediate_drawer_) {
    return;
  }
  if (presenter_) {
    presenter_->RemoveUIDrawerFromUIThread(this);
  }
  // Texture is owned by the immediate_drawer's resources, drop it before
  // the immediate_drawer goes away.
  texture_.reset();
  texture_width_ = 0;
  texture_height_ = 0;
  presenter_ = presenter;
  immediate_drawer_ = immediate_drawer;
  if (presenter_) {
    presenter_->AddUIDrawerFromUIThread(this, kBinkUIDrawerZOrder);
  }
}

void BinkUIDrawer::Draw(UIDrawContext& context) {
  if (!immediate_drawer_) return;
  auto* overlay = rex::system::BinkOverlay::Get();
  if (!overlay->HasActiveFmv()) {
    // Free the texture when no FMV is active to avoid GPU memory pinning.
    texture_.reset();
    texture_width_ = 0;
    texture_height_ = 0;
    last_pts_us_ = -1;
    return;
  }

  rex::system::BinkOverlayFrame frame{};
  if (!overlay->AcquireCurrentFrame(frame) || frame.data == nullptr ||
      frame.width == 0 || frame.height == 0) {
    return;
  }

  // Recreate the host texture only when the decoder has produced a new
  // frame. The drawer fires once per presented swap (~30-60 Hz host) but
  // bink video plays at ~24 fps, so most calls would re-upload identical
  // RGBA bytes. At 1280×720 RGBA each upload is 3.7 MB; doing it on every
  // present saturates the eMMC/UMA upload path and drops video pacing to
  // a crawl. Reuse the existing texture when presentation_us is unchanged.
  bool dims_changed = !texture_ || frame.width != texture_width_ ||
                      frame.height != texture_height_;
  bool new_frame = dims_changed || frame.presentation_us != last_pts_us_;
  if (new_frame) {
    texture_ = immediate_drawer_->CreateTexture(
        frame.width, frame.height, ImmediateTextureFilter::kLinear,
        /*is_repeated=*/false, frame.data);
    texture_width_ = frame.width;
    texture_height_ = frame.height;
    last_pts_us_ = frame.presentation_us;
  }
  if (!texture_) return;

  // Draw a full-window letterboxed quad. Use the render target as the
  // coordinate space so positions are in pixels.
  const float rt_w = static_cast<float>(context.render_target_width());
  const float rt_h = static_cast<float>(context.render_target_height());
  if (rt_w <= 0.f || rt_h <= 0.f) return;

  const float src_aspect = static_cast<float>(frame.width) /
                           static_cast<float>(frame.height);
  const float dst_aspect = rt_w / rt_h;
  float draw_w, draw_h;
  if (src_aspect >= dst_aspect) {
    // Letterbox top/bottom.
    draw_w = rt_w;
    draw_h = rt_w / src_aspect;
  } else {
    // Pillarbox left/right.
    draw_h = rt_h;
    draw_w = rt_h * src_aspect;
  }
  const float dx = (rt_w - draw_w) * 0.5f;
  const float dy = (rt_h - draw_h) * 0.5f;
  const uint32_t white = 0xFFFFFFFFu;
  ImmediateVertex verts[4] = {
      {dx,           dy,            0.f, 0.f, white},
      {dx + draw_w,  dy,            1.f, 0.f, white},
      {dx + draw_w,  dy + draw_h,   1.f, 1.f, white},
      {dx,           dy + draw_h,   0.f, 1.f, white},
  };
  const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};

  immediate_drawer_->Begin(context, rt_w, rt_h);

  ImmediateDrawBatch batch{};
  batch.vertices = verts;
  batch.vertex_count = 4;
  batch.indices = indices;
  batch.index_count = 6;
  immediate_drawer_->BeginDrawBatch(batch);

  ImmediateDraw draw{};
  draw.primitive_type = ImmediatePrimitiveType::kTriangles;
  draw.count = 6;
  draw.texture = texture_.get();
  immediate_drawer_->Draw(draw);

  immediate_drawer_->EndDrawBatch();
  immediate_drawer_->End();
}

}  // namespace rex::ui
