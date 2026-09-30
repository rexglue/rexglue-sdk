/**
 * @file        ui/overlay/console_overlay.h
 * @brief       Console overlay docked in the title window
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class OverlayContext;

/** The console docked at the top left of the title window, resizable on its right and bottom. */
class ConsoleDialog : public ImGuiDialog {
 public:
  /**
   * Builds the dialog over the context's console model.
   *
   * @param context What the overlay may use.
   */
  explicit ConsoleDialog(const OverlayContext& context);

  /**
   * Shows or hides the OS window instead of the dialog while the console is undocked.
   *
   * @return True when undocked.
   */
  bool OnToggle() override;

 protected:
  /** Focuses the input. */
  void OnShow() override;

  /**
   * Draws the docked or minimized console.
   *
   * @param io The drawer's ImGui IO.
   */
  void OnDraw(ImGuiIO& io) override;

 private:
  void ExecuteCommand(std::string_view cmd);
  void AddLocal(rex::log::Level level, std::string text);
  void RefreshCategories();
  static int InputTextCallback(ImGuiInputTextCallbackData* data);
  void UpdateCompletionCandidates(const char* buf, int len);
  void ApplyCompletion(ImGuiInputTextCallbackData* data);
  void HandleHistoryOrCompletionNav(ImGuiInputTextCallbackData* data);

  bool focus_input_next_frame_ = true;
  bool scroll_to_bottom_ = true;
  float console_height_ = 0.0f;  // 0 = initialize to default fraction on first draw
  uint64_t last_generation_ = 0;

  // Console-only output (command feedback). Kept out of the log sink/files, but
  // tagged with the sink generation at the moment it was produced so it can be
  // interleaved chronologically with the captured log lines at draw time.
  struct LocalEntry {
    rex::log::CapturedRecord entry;
    uint64_t seq;  // sink generation when produced
  };

  rex::cvar::ICvarRegistry& registry_;
  rex::log::CaptureSink* sink_;
  std::vector<rex::log::CapturedRecord>
      entries_;  // sink snapshot, refreshed when generation changes
  std::vector<LocalEntry> local_entries_;

  // Filters
  int min_level_ = 0;
  std::set<std::string> known_categories_;
  std::map<std::string, bool> category_filter_;  // category -> enabled

  // Command input
  char input_buf_[512] = {};
  std::deque<std::string> history_;
  int history_pos_ = -1;
  static constexpr size_t kMaxHistory = 32;

  // Autocomplete over cvar/command names.
  std::vector<std::string> completion_candidates_;
  int completion_index_ = -1;  // selected popup item, -1 = none highlighted
  bool completion_open_ = false;
};

}  // namespace rex::ui
