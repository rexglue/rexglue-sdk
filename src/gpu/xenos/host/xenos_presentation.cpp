#include "gpu/xenos/host/xenos_presentation.h"

#include <utility>

#include <imgui.h>

#include "gpu/xenos/host/immediate_drawer.h"

namespace rex::gpu {

XenosPresentation::XenosPresentation(const NativeWindow& window) : surface_(window) {}

std::unique_ptr<XenosPresentation> XenosPresentation::Create(
    const NativeWindow& window, GraphicsProvider& provider, const XenosGraphicsDriverBase& driver,
    Presenter::HostGPULossCallback loss_callback) {
  std::unique_ptr<XenosPresentation> presentation(new XenosPresentation(window));
  presentation->presenter_ = provider.CreatePresenter(driver, std::move(loss_callback));
  if (!presentation->presenter_) {
    return nullptr;
  }
  presentation->drawer_ = provider.CreateImmediateDrawer();
  if (!presentation->drawer_) {
    return nullptr;
  }
  presentation->drawer_->SetPresenter(presentation->presenter_.get());
  presentation->overlay_ = std::make_unique<ImGuiOverlay>(*presentation->drawer_);
  presentation->presenter_->AddOverlayLayer(presentation.get(), 0);
  return presentation;
}

XenosPresentation::~XenosPresentation() {
  if (presenter_) {
    presenter_->RemoveOverlayLayer(this);
    presenter_->Attach(nullptr, nullptr);
  }
  overlay_.reset();
  if (drawer_) {
    drawer_->SetPresenter(nullptr);
  }
  drawer_.reset();
  presenter_.reset();
}

void XenosPresentation::Resize(u32 width, u32 height) {
  surface_.SetSize(width, height);
  if (attached_) {
    presenter_->OnSurfaceResize();
  }
}

bool XenosPresentation::ConnectToWindow() {
  u32 width = 0;
  u32 height = 0;
  if (!surface_.GetSize(width, height)) {
    return false;
  }
  if (!attached_) {
    attached_ = presenter_->Attach(this, &surface_);
  }
  return attached_;
}

void XenosPresentation::SetOverlays(ImDrawData& draw_data) {
  overlay_->UpdateTextures(draw_data);
  draw_data_ = &draw_data;
}

void XenosPresentation::Present() {
  presenter_->Paint(true);
  draw_data_ = nullptr;
}

void XenosPresentation::Draw(OverlayDrawContext& context) {
  if (draw_data_ != nullptr) {
    overlay_->Render(context, *draw_data_);
  }
}

}  // namespace rex::gpu
