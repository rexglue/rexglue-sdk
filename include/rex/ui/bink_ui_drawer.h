#ifndef REX_UI_BINK_UI_DRAWER_H_
#define REX_UI_BINK_UI_DRAWER_H_

// Cinematic overlay drawer.
//
// Pulls decoded RGBA frames from rex::system::BinkOverlay and composites
// them as a full-window quad over Kameo's swap. Registered with the
// Presenter as a UIDrawer at a low z-order so the ImGui overlays still
// draw on top. While there's no active FMV, Draw() is a no-op.

#include <memory>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/ui_drawer.h>

namespace rex::ui {

class Presenter;

class BinkUIDrawer : public UIDrawer {
 public:
  BinkUIDrawer();
  ~BinkUIDrawer();

  // Attaches to the presenter and immediate drawer. Call from the UI
  // thread once both are valid; pass nullptr to detach.
  void SetPresenter(Presenter* presenter, ImmediateDrawer* immediate_drawer);

  // UIDrawer
  void Draw(UIDrawContext& context) override;

 private:
  Presenter* presenter_ = nullptr;
  ImmediateDrawer* immediate_drawer_ = nullptr;

  std::unique_ptr<ImmediateTexture> texture_;
  uint32_t texture_width_ = 0;
  uint32_t texture_height_ = 0;
  int64_t last_pts_us_ = -1;
};

}  // namespace rex::ui

#endif  // REX_UI_BINK_UI_DRAWER_H_
