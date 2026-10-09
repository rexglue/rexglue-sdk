/**
 * @file        core/app_state.h
 * @brief       Process wide state behind the shutdown request functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::detail {

/** Clears every request. App::Init calls it. */
REX_API void ResetAppState();

/**
 * Sets the function RequestShutdown calls after storing the request, so a waiting loop
 * wakes.
 *
 * @param wake Function to call, or an empty function for none.
 */
REX_API void SetShutdownWake(std::move_only_function<void()> wake);

/**
 * Returns the exit code of the first shutdown request.
 *
 * @return The code, or std::nullopt when no shutdown was requested.
 */
REX_API std::optional<i32> RequestedExitCode();

/**
 * Takes the arguments of a restart request.
 *
 * @return The arguments, empty for the current ones, or std::nullopt when no restart was
 *         requested.
 */
REX_API std::optional<std::vector<std::string>> TakeRestartArgs();

/** Signature of the function that starts the new process. */
using RelaunchFunction = Result<void> (*)(std::span<const std::string> args);

/**
 * Replaces the relaunch for tests.
 *
 * @param relaunch Function to call, or nullptr for platform::Relaunch.
 * @param can_relaunch Value CanRelaunchProcess returns while relaunch is replaced.
 */
REX_API void SetRelaunchForTest(RelaunchFunction relaunch, bool can_relaunch);

/**
 * Starts the new process through platform::Relaunch or the test replacement.
 *
 * @param args Arguments for the new process.
 * @return The result of the relaunch.
 */
[[nodiscard]] REX_API Result<void> RelaunchProcess(std::span<const std::string> args);

/**
 * Reports whether a relaunch may run.
 *
 * @return platform::CanRelaunch, or the test value while relaunch is replaced.
 */
REX_API bool CanRelaunchProcess();

}  // namespace rex::detail
