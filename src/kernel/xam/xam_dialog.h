/**
 * @file        kernel/xam/xam_dialog.h
 * @brief       Creation of XAM dialogs on the UI thread
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>

#include <rex/platform/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>

namespace rex::kernel::xam {

/**
 * Creates a dialog on the UI thread and attaches a fence that the dialog's destruction signals.
 *
 * @param context The app context whose UI thread owns the drawer.
 * @param create Builds the dialog. Runs on the UI thread, and never runs once the UI loop quit.
 * @param fence Signaled when the dialog closes or the drawer destroys it.
 * @return True when the dialog was created, false when the UI thread runs no more functions.
 */
bool ShowDialog(ui::WindowedAppContext* context, const std::function<ui::ImGuiDialog*()>& create,
                platform::Fence& fence);

}  // namespace rex::kernel::xam
