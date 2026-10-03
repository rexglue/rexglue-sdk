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

#ifndef REX_UI_IMGUI_DRAWER_H_
#define REX_UI_IMGUI_DRAWER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <rex/gpu/overlay.h>
#include <rex/gpu/presenter.h>
#include <rex/ui/style.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace rex {
namespace ui {

class ImGuiDialog;
class Window;

class ImGuiDrawer : public WindowInputListener, public gpu::IOverlayLayer {
 public:
  using FontSetupCallback = std::function<void(ImFontAtlas*)>;
  using StyleSetupCallback = std::function<void(ImGuiStyle&, Style&)>;
  ImGuiDrawer(Window* window, size_t z_order, FontSetupCallback font_setup = nullptr,
              StyleSetupCallback style_setup = nullptr);
  ~ImGuiDrawer();

  ImGuiIO& GetIO();

  /**
   * Returns the style of this drawer's ImGui context.
   *
   * @return The style every dialog of this drawer draws with.
   */
  ImGuiStyle& imgui_style();

  // Per-overlay styling, patched by the consumer in OnConfigureStyle.
  Style& style() { return style_; }
  const Style& style() const { return style_; }

  void AddDialog(ImGuiDialog* dialog);
  void RemoveDialog(ImGuiDialog* dialog);

  /**
   * Destroys every registered dialog the drawer owns, which signals the fences waiting on them.
   * Owned dialogs stay with their owner.
   */
  void DestroyDialogs();

  // Whether Draw would render anything, so a caller that has to marshal to the
  // UI thread can skip the round trip entirely.
  bool HasDialogs() const { return !dialogs_.empty(); }

  // SetPresenter may be called from the destructor.
  void SetPresenter(gpu::IPresenter* new_presenter);
  void SetOverlayDrawer(gpu::IOverlayDrawer* new_overlay_drawer);
  void SetPresenterAndOverlayDrawer(gpu::IPresenter* new_presenter,
                                    gpu::IOverlayDrawer* new_overlay_drawer) {
    SetPresenter(new_presenter);
    SetOverlayDrawer(new_overlay_drawer);
  }

  void Draw(gpu::OverlayDrawContext& context) override;

 protected:
  void OnKeyDown(KeyEvent& e) override;
  void OnKeyUp(KeyEvent& e) override;
  void OnKeyChar(KeyEvent& e) override;
  void OnMouseDown(MouseEvent& e) override;
  void OnMouseMove(MouseEvent& e) override;
  void OnMouseUp(MouseEvent& e) override;
  void OnMouseWheel(MouseEvent& e) override;
  void OnTouchEvent(TouchEvent& e) override;
  // For now, no need for OnDpiChanged because redrawing is done continuously.

 private:
  void Initialize();

  void SetupFontTexture();

  void RenderDrawLists(ImDrawData* data, gpu::OverlayDrawContext& context);

  void ClearInput();
  void OnKey(KeyEvent& e, bool is_down);
  void UpdateMousePosition(float x, float y);
  void SwitchToPhysicalMouseAndUpdateMousePosition(const MouseEvent& e);

  bool IsDrawingDialogs() const { return dialog_loop_next_index_ != SIZE_MAX; }
  void DetachIfLastDialogRemoved();

  std::optional<ImGuiKey> VirtualKeyToImGuiKey(hid::VirtualKey vkey);

  Window* window_;
  size_t z_order_;
  FontSetupCallback font_setup_;
  StyleSetupCallback style_setup_;
  Style style_;

  ImGuiContext* internal_state_ = nullptr;

  // All currently-attached dialogs that get drawn.
  std::vector<ImGuiDialog*> dialogs_;
  // Using an index, not an iterator, because after the erasure, the adjustment
  // must be done for the vector element indices that would be in the iterator
  // range that would be invalidated.
  // SIZE_MAX if not currently in the dialog loop.
  size_t dialog_loop_next_index_ = SIZE_MAX;

  gpu::IPresenter* presenter_ = nullptr;

  gpu::IOverlayDrawer* overlay_drawer_ = nullptr;
  // Resources specific to an overlay drawer - must be destroyed before
  // detaching the presenter.
  std::unique_ptr<gpu::OverlayTexture> font_texture_;

  // If there's an active pointer, the ImGui mouse is controlled by this touch.
  // If it's TouchEvent::kPointerIDNone, the ImGui mouse is controlled by the
  // mouse.
  uint32_t touch_pointer_id_ = TouchEvent::kPointerIDNone;
  // Whether after the next frame (since the mouse up event needs to be handled
  // with the correct mouse position still), the ImGui mouse position should be
  // reset (for instance, after releasing a touch), so it's not hovering over
  // anything.
  bool reset_mouse_position_after_next_frame_ = false;

  double frame_time_tick_frequency_;
  uint64_t last_frame_time_ticks_;

  // ImGui's IME hook, called from EndFrame with the frame being ended.
  // ImGuiIO::WantTextInput would be a frame behind.
  static void PlatformSetImeData(ImGuiContext* context, ImGuiViewport* viewport,
                                 ImGuiPlatformImeData* data);
  void SetWindowTextInputActive(bool active);

  // Last value handed to the window, so detaching can clear it without a frame.
  bool text_input_active_ = false;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_UI_IMGUI_DRAWER_H_
