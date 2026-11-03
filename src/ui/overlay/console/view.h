/**
 * @file        ui/overlay/console/view.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/utility.h>
#include "ui/overlay/console/model.h"

struct ImGuiInputTextCallbackData;

namespace rex::ui {

struct ConsoleStyle;

/** Dock changes a console view asks its owner for. The owner applies them after the frame. */
class IConsoleHost : public NonCopyable {
 public:
  virtual ~IConsoleHost() = default;

  /**
   * Reports whether the console can move to its own OS window.
   *
   * @return True when the graphics driver can present a second window.
   */
  virtual bool CanUndock() const = 0;

  /** Moves the console to its own OS window. */
  virtual void Undock() = 0;

  /** Moves the console back into the title window. */
  virtual void Dock() = 0;

  /** Hides the console, docked or undocked. */
  virtual void Hide() = 0;

  /** Shows or hides the undocked OS window. */
  virtual void ToggleWindow() = 0;
};

/** Draws a console model into the current ImGui window. */
class ConsoleView : public NonCopyable {
 public:
  /**
   * Builds a view.
   *
   * @param model The model drawn and edited. Outlives the view.
   * @param host Receives dock changes. Outlives the view.
   */
  ConsoleView(ConsoleModel& model, IConsoleHost& host);

  /**
   * Draws the filter row, the output, the completion popup, the input and the help line.
   *
   * @param style Colors of the levels and the completion popup.
   */
  void Draw(const ConsoleStyle& style);

  /** Moves keyboard focus to the input on the next Draw. */
  void FocusInput() { focus_input_ = true; }

 private:
  static int InputCallback(ImGuiInputTextCallbackData* data);
  void DrawFilters();
  void DrawOutput(const ConsoleStyle& style);
  void DrawCompletions(const ConsoleStyle& style, ImVec2 input_min, f32 width);
  void CompleteInput(ImGuiInputTextCallbackData* data);
  void Navigate(ImGuiInputTextCallbackData* data);
  void AcceptSelected(const std::string& name);
  void CloseCompletions();
  std::string SelectedText(bool all) const;

  ConsoleModel& model_;
  IConsoleHost& host_;
  ImGuiSelectionBasicStorage selection_;
  std::vector<const ConsoleLine*> visible_;
  std::vector<Completion> completions_;
  i32 highlight_ = -1;
  bool edited_ = false;
  bool dismissed_ = false;
  bool focus_input_ = true;
  bool input_active_ = false;
  bool scroll_to_bottom_ = true;
};

}  // namespace rex::ui
