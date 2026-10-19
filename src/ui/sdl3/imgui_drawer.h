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

#ifndef REX_UI_SDL3_IMGUI_DRAWER_H
#define REX_UI_SDL3_IMGUI_DRAWER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <imgui.h>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/gpu/overlay.h>
#include <rex/gpu/presenter.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/move_only_function.h>
#include "ui/sdl3/window.h"
#include "ui/sdl3/window_listener.h"

namespace rex {
namespace ui {

class Window;

class ImGuiDrawer : public WindowInputListener, public gpu::IOverlayLayer, public IDialogHost {
 public:
  using FontSetupCallback = rex::MoveOnlyFunction<void(ImFontAtlas*)>;
  ImGuiDrawer(Window* window, size_t z_order, FontSetupCallback font_setup = nullptr);
  ~ImGuiDrawer();

  ImGuiIO& GetIO();

  /**
   * Returns the style of this drawer's ImGui context.
   *
   * @return The style every dialog of this drawer draws with.
   */
  ImGuiStyle& GetImGuiStyle();

  /**
   * Draws a dialog the caller owns. Adding an attached dialog does nothing.
   *
   * @param dialog The dialog. The caller keeps it alive until it is removed or destroyed.
   */
  void AddDialog(ImGuiDialog& dialog);

  /**
   * Draws a dialog the drawer owns. The drawer destroys it after the frame that drew its close,
   * or in DestroyDialogs.
   *
   * @param dialog The dialog. A dialog another host draws is destroyed at once.
   */
  void AddDialog(std::unique_ptr<ImGuiDialog> dialog);

  /**
   * Stops drawing a dialog and detaches it. A dialog the drawer owns is destroyed after the draw
   * loop, or at once when no loop runs.
   *
   * @param dialog The dialog to remove. An unknown dialog does nothing.
   */
  void RemoveDialog(ImGuiDialog& dialog) override;

  /**
   * Destroys every dialog the drawer owns, which runs their destructors, and detaches every
   * other dialog. Must not run inside a draw.
   */
  void DestroyDialogs();

  /**
   * Destroys every dialog the drawer owns and leaves the dialogs their callers own attached. Must
   * not run inside a draw.
   */
  void DestroyOwnedDialogs();

  /**
   * Reports whether Draw would render anything.
   *
   * @return True while at least 1 dialog is attached.
   */
  bool HasDialogs() const { return !dialogs_.empty(); }
  void SetDialogCounter(std::atomic<u32>* counter);

  /**
   * Sets the presenter. May be called from the destructor.
   *
   * @param new_presenter Presenter to attach, or null to detach.
   */
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

 private:
  void Initialize();

  void SetupFontTexture();

  void RenderDrawLists(ImDrawData* data, gpu::OverlayDrawContext& context);

  void ClearInput();
  void OnKey(KeyEvent& e, bool is_down);
  void UpdateMousePosition(f32 x, f32 y);
  void SwitchToPhysicalMouseAndUpdateMousePosition(const MouseEvent& e);

  bool IsDrawingDialogs() const { return dialog_loop_next_index_ != SIZE_MAX; }
  void DetachIfLastDialogRemoved();
  void PublishDialogCount();

  std::optional<ImGuiKey> VirtualKeyToImGuiKey(hid::VirtualKey vkey);

  Window* window_;
  size_t z_order_;
  FontSetupCallback font_setup_;

  ImGuiContext* internal_state_ = nullptr;

  std::vector<ImGuiDialog*> dialogs_;
  std::atomic<u32>* dialog_counter_ = nullptr;
  std::vector<std::unique_ptr<ImGuiDialog>> owned_;
  std::vector<std::unique_ptr<ImGuiDialog>> retired_;
  /**
   * Index of the next dialog to draw, SIZE_MAX outside the dialog loop. An index is used because
   * erasure invalidates iterators.
   */
  size_t dialog_loop_next_index_ = SIZE_MAX;

  gpu::IPresenter* presenter_ = nullptr;

  gpu::IOverlayDrawer* overlay_drawer_ = nullptr;
  /** Overlay drawer resources, destroyed before the presenter detaches. */
  std::unique_ptr<gpu::OverlayTexture> font_texture_;

  /** Pointer id of the touch controlling the ImGui mouse, kPointerIDNone when the mouse does. */
  u32 touch_pointer_id_ = TouchEvent::kPointerIDNone;
  /**
   * True to reset the ImGui mouse position after the next frame, so the mouse up event is handled
   * at the correct position first and the mouse then hovers over nothing.
   */
  bool reset_mouse_position_after_next_frame_ = false;

  f64 frame_time_tick_frequency_;
  u64 last_frame_time_ticks_;

  /**
   * ImGui IME hook, called from EndFrame with the frame being ended. WantTextInput lags a frame.
   *
   * @param context ImGui context.
   * @param viewport Viewport being updated.
   * @param data IME data for the frame.
   */
  static void PlatformSetImeData(ImGuiContext* context, ImGuiViewport* viewport,
                                 ImGuiPlatformImeData* data);
  void SetWindowTextInputActive(bool active);

  /** Last value handed to the window, so detaching can clear it without a frame. */
  bool text_input_active_ = false;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_UI_SDL3_IMGUI_DRAWER_H
