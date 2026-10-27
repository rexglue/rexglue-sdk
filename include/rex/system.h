/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/string.h>

namespace rex {

#if REX_PLATFORM_ANDROID
bool InitializeAndroidSystemForApplicationContext();
void ShutdownAndroidSystem();
#endif

// Open a URL or folder in the desktop shell and log a warning on failure.
// Windows opens them on a background thread; elsewhere SDL_OpenURL runs on
// the calling thread, which must be the main thread.
// The URL must include the protocol.
void LaunchWebBrowser(const std::string_view url);
// A path to an existing file opens its containing folder.
void LaunchFileExplorer(const std::filesystem::path& path);

enum class SimpleMessageBoxType {
  Help,
  Warning,
  Error,
};

// Thread-safe window title for ShowSimpleMessageBox. Empty restores the platform default.
void SetMessageBoxTitle(std::string_view title);
std::string GetMessageBoxTitle();

// This is expected to block the caller until the message box is closed.
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message);

}  // namespace rex
