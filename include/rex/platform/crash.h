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

#include <span>
#include <string>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::platform {

/** Options for the process crash reporter. */
struct CrashReporterOptions {
  /** Name in the report banner and the dialog. Empty names the executable. */
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::string app_name = {};
  /** Shows a message box after the report is written. */
  bool show_dialog = false;
};

/**
 * Describes a host fault address in title terms. Runs inside a signal handler, so it neither
 * allocates nor locks.
 *
 * @param host_address The host fault address, or 0 when the fault has none.
 * @param out The buffer the text goes into.
 * @return The length of the text, 0 for none.
 */
using CrashAddressDescriber = u32 (*)(u64 host_address, std::span<char> out);

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
 * Reserves the stack room a report needs on the calling thread, so stack overflow reports work.
 */
REX_API void PrepareThreadForCrash();

}  // namespace rex::platform
