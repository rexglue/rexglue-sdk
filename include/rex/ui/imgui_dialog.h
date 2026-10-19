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

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>
#include <rex/module.h>

struct ImGuiIO;

namespace rex::ui {

class ImGuiDialog;

/** Owner of a set of dialogs that draws them each frame and removes a dialog that closed. */
REX_INTERFACE(IDialogHost)
 public:
  /**
   * Stops drawing a dialog and detaches it. Called by a dialog that closed during Draw and by a
   * dialog that is destroyed while attached.
   *
   * @param dialog The dialog to remove.
   */
  virtual void RemoveDialog(ImGuiDialog& dialog) = 0;
};

/**
 * A window that an IDialogHost draws with ImGui. A dialog never adds itself and never deletes
 * itself, its owner creates it, hands it to a host and destroys it. Every hook runs on the UI
 * thread.
 */
class REX_IMGUI_API ImGuiDialog : public NonCopyable {
 public:
  /** Removes the dialog from its host when it is attached. */
  virtual ~ImGuiDialog();

  /**
   * Draws the dialog. When Close was requested during the draw, marks the dialog closed, calls
   * OnClose once and asks the host to remove it.
   */
  void Draw();

  /**
   * Reports whether a drawn Close request completed.
   *
   * @return True once a Close request was drawn.
   */
  bool IsClosed() const;

  /**
   * Reports whether a host draws the dialog.
   *
   * @return True while a host holds the dialog.
   */
  bool IsAttached() const;

  /**
   * Sets the host that draws the dialog. Only a host calls this.
   *
   * @param host The host, or null to detach.
   */
  void Attach(IDialogHost* host);

  /** Called when an overlay registry shows the dialog. */
  virtual void OnShow() {}

  /**
   * Lets an overlay handle its toggle bind itself while hidden.
   *
   * @return True when the dialog handled the toggle and stays hidden.
   */
  virtual bool OnToggle() { return false; }

 protected:
  ImGuiDialog() = default;

  /** Requests that the next Draw closes the dialog. */
  void Close();

  /** Called once from Draw after a Close request, before the host removes the dialog. */
  virtual void OnClose() {}

  /**
   * Draws the contents of the dialog.
   *
   * @param io The IO of the current ImGui context.
   */
  virtual void OnDraw(ImGuiIO& io) { (void)io; }

 private:
  IDialogHost* host_ = nullptr;
  bool close_pending_ = false;
  bool closed_ = false;
};

}  // namespace rex::ui
