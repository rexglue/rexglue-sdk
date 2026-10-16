/**
 * @file        ui/sdl3/overlay/debug_overlay.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/ui/imgui_dialog.h>
#include <array>
#include <cstdint>
#include <functional>

namespace rex::ui {

struct ModuleOverlayContext;

class DebugOverlayDialog : public ImGuiDialog {
 public:
  explicit DebugOverlayDialog(const ModuleOverlayContext& context);
  ~DebugOverlayDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  const ModuleOverlayContext& context_;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  static constexpr size_t kFrameHistorySize = 120;
  std::array<f32, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
#endif
};

}  // namespace rex::ui
