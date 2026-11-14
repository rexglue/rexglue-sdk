/**
 * @file        app/lifecycle.h
 * @brief       Shutdown and restart requests, the exit code, relaunch and signal handlers (private)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::app::detail {

/** Clears every request. */
void ResetAppState();

/**
 * Returns the exit code of the first shutdown request, or std::nullopt when no shutdown was
 * requested.
 */
std::optional<i32> GetRequestedExitCode();

/**
 * Takes the arguments of a restart request.
 *
 * @return The arguments, empty for the current ones, or std::nullopt when no restart was
 *         requested.
 */
std::optional<std::vector<std::string>> TakeRestartArgs();

using RelaunchFunction = Result<void> (*)(std::span<const std::string> args);

/**
 * Replaces the relaunch for tests.
 *
 * @param relaunch Function to call, or nullptr for platform::Relaunch.
 * @param can_relaunch Value CanRelaunchProcess returns while relaunch is replaced.
 */
void SetRelaunchForTest(RelaunchFunction relaunch, bool can_relaunch);

/** Starts the new process through platform::Relaunch or the test replacement. */
[[nodiscard]] Result<void> RelaunchProcess(std::span<const std::string> args);

/** Reports whether a relaunch may run. */
bool CanRelaunchProcess();

using StartupBoxFunction = void (*)(MessageType type, std::string_view message);

/**
 * Replaces the message box of a failed startup for tests.
 *
 * @param box Function to call, or nullptr for rex::ShowMessageBox.
 */
void SetStartupBoxForTest(StartupBoxFunction box);

/** Shows the message box of a failed startup through rex::ShowMessageBox or the test replacement.
 */
void ShowStartupBox(MessageType type, std::string_view message);

/**
 * Sets the app that ran rex::Init last.
 *
 * @param owner The app, as an identity only.
 */
void SetInitOwner(const void* owner);

/**
 * Clears the init owner when it is the one given.
 *
 * @param owner The app, as an identity only.
 * @return True when it was the init owner, so its teardown owns the rex::Init session.
 */
[[nodiscard]] bool ReleaseInitOwner(const void* owner);

/** Installs the handlers that turn a console or termination signal into a shutdown request. */
void InstallSignalHandlers();

/** Removes the handlers InstallSignalHandlers installed. */
void RemoveSignalHandlers();

/** Unblocks the console close handler that waits for the teardown. */
void FinishTeardown();

/**
 * Sets the function RequestShutdown calls to wake the frame loop.
 *
 * @param wake The function, or an empty one for none.
 */
void SetShutdownWake(rex::MoveOnlyFunction<void()> wake);

}  // namespace rex::app::detail
