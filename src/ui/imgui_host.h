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

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <rex/utility.h>
#include <rex/types.h>
#include <rex/ui/ui_system.h>
#include <rex/ui/ui_event.h>

#include "ui/input_capture.h"

namespace rex::ui {

class ImGuiHost final {
 public:
  struct Hooks {
    MoveOnlyFunction<void(bool)> set_text_input;
    MoveOnlyFunction<void(bool)> set_mouse_capture;
    MoveOnlyFunction<std::string()> get_clipboard;
    MoveOnlyFunction<void(std::string_view)> set_clipboard;
  };

  explicit ImGuiHost(Hooks hooks);
  ~ImGuiHost();

  ImGuiIO& GetIO();
  ImGuiStyle& GetImGuiStyle();
  ImFontAtlas& GetFonts();
  const InputCapture& GetInputCapture() const { return capture_; }

  u64 AddDialog(IDialog& dialog);
  void AddDialog(std::unique_ptr<IDialog> dialog);
  void RemoveDialog(IDialog& dialog, u64 id = 0);
  bool Contains(const IDialog& dialog) const;
  void DestroyDialogs();
  void DestroyOwnedDialogs();
  bool HasDialogs() const { return !dialogs_.empty(); }
  void SetDialogCounter(std::atomic<u32>* counter);

  void SetDisplay(u32 physical_width, u32 physical_height, f32 content_scale);
  bool HandleEvent(UIEvent& event);
  void HandleEvent(InputEvent& event);
  ImDrawData* BuildFrame();
  bool NeedsContinuousPaint() const;

 private:
  void Initialize();
  void ClearInput();
  void OnKey(InputEvent& e, bool is_down);
  void OnKeyChar(InputEvent& e);
  void OnMouseButton(const InputEvent& e, bool is_down);
  void OnTouch(TouchEvent& e);
  void UpdateMousePosition(f32 x, f32 y);
  void SwitchToPhysicalMouseAndUpdateMousePosition(const InputEvent& e);
  bool IsDrawingDialogs() const { return dialog_loop_next_index_ != SIZE_MAX; }
  void DetachIfLastDialogRemoved();
  void PublishDialogCount();
  void SetTextInputActive(bool active);
  static void PlatformSetImeData(ImGuiContext* context, ImGuiViewport* viewport,
                                 ImGuiPlatformImeData* data);

  Hooks hooks_;
  ImGuiContext* context_ = nullptr;
  InputCapture capture_;
  struct DialogEntry {
    IDialog* dialog;
    u64 id;
  };

  std::vector<DialogEntry> dialogs_;
  u64 next_dialog_id_ = 1;
  std::atomic<u32>* dialog_counter_ = nullptr;
  std::vector<std::unique_ptr<IDialog>> owned_;
  std::vector<std::unique_ptr<IDialog>> retired_;
  size_t dialog_loop_next_index_ = SIZE_MAX;
  u32 touch_pointer_id_ = TouchEvent::kPointerIDNone;
  bool reset_mouse_position_after_next_frame_ = false;
  bool text_input_active_ = false;
  bool changed_ = false;
  u32 display_width_ = 0;
  u32 display_height_ = 0;
  f32 content_scale_ = 1.0f;
  f64 frame_time_tick_frequency_ = 1.0;
  u64 last_frame_time_ticks_ = 0;
};

}  // namespace rex::ui
