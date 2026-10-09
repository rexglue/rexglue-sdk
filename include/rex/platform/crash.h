/**
 * @file        platform/crash.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::platform {

/** Options for the process crash reporter. */
struct CrashReporterOptions {
  /** Name in the report banner and the dialog. Empty names the executable. */
  std::string app_name = {};
  /** Shows a message box after the report is written. */
  bool show_dialog = false;
};

/** Describes a host fault address in title terms, or returns an empty string. */
using CrashAddressDescriber = std::string (*)(u64 host_address);

/**
 * Installs the crash reporter for the process, or replaces its options when it is installed.
 * Prepares the calling thread.
 *
 * @param options The app name and dialog choice of the report.
 * @return Ok, or a Platform error when a handler cannot be installed.
 */
[[nodiscard]] REX_API Result<void> InstallCrashReporter(const CrashReporterOptions& options);

/** Removes the crash reporter and restores the handlers it replaced. */
REX_API void UninstallCrashReporter();

/**
 * Sets the function the report calls to describe the fault address and the faulting thread.
 *
 * @param describer Names a host fault address in title terms, or null for none.
 */
REX_API void SetCrashAddressDescriber(CrashAddressDescriber describer);

/**
 * Reserves the stack room a report needs on the calling thread. Thread starts in the SDK call it.
 * A thread the SDK did not start calls it to keep stack overflow reports.
 */
REX_API void PrepareThreadForCrash();

}  // namespace rex::platform
