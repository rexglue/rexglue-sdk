/**
 * @file        gpu/xenos/host/xenos_presentation.h
 * @brief       Presents a Xenos driver into a UISystem window
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include "gpu/xenos/xenos_export.h"
#include <rex/platform/native_window.h>
#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/imgui_overlay.h"
#include "gpu/xenos/host/presentation.h"
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/host/surface.h"

struct ImDrawData;

namespace rex::gpu {

class XenosGraphicsDriverBase;

/**
 * Owns the presenter, overlay drawer and surface of a Xenos driver. Every method after Create
 * runs on the present thread, which is the presenter UI thread.
 */
class REX_GPU_XENOS_API XenosPresentation final : public IPresentationHost, public IOverlayLayer {
 public:
  /**
   * Builds the presenter and the drawer.
   *
   * @param window The window to present into.
   * @param provider The provider of the host API.
   * @param driver The driver the presenter paints for.
   * @param loss_callback Called on a host GPU loss.
   * @return The presentation, or null when the provider fails to create either object.
   */
  static std::unique_ptr<XenosPresentation> Create(const NativeWindow& window,
                                                   GraphicsProvider& provider,
                                                   const XenosGraphicsDriverBase& driver,
                                                   Presenter::HostGPULossCallback loss_callback);
  ~XenosPresentation() override;

  /** Returns the presenter the command processor refreshes guest output on. */
  Presenter* GetPresenter() const { return presenter_.get(); }

  /**
   * Sets the size of the window and notifies the presenter.
   *
   * @param width Width in pixels.
   * @param height Height in pixels.
   */
  void Resize(u32 width, u32 height);

  /**
   * Connects the presenter to the window once.
   *
   * @return True when the presenter can paint.
   */
  bool ConnectToWindow();

  /**
   * Uploads the overlay textures and keeps the frame for the next paint.
   *
   * @param draw_data The frame, valid until Present returns.
   */
  void SetOverlays(ImDrawData& draw_data);

  void Present();

  void RequestPaint() override {}
  void OnPresenterDetached() override {}
  void Draw(OverlayDrawContext& context) override;

 private:
  XenosPresentation(const NativeWindow& window);

  NativeSurface surface_;
  std::unique_ptr<Presenter> presenter_;
  std::unique_ptr<OverlayDrawer> drawer_;
  std::unique_ptr<ImGuiOverlay> overlay_;
  ImDrawData* draw_data_ = nullptr;
  bool attached_ = false;
};

}  // namespace rex::gpu
