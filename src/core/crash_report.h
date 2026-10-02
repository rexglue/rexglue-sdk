/**
 * @file        core/crash_report.h
 * @brief       Report writer and platform hooks shared by the crash reporter sources
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <rex/exception_handler.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

#if !REX_PLATFORM_WIN32
#include <signal.h>
#endif

namespace rex::platform::detail {

/** A fault as the platform source captured it. */
struct FaultReport {
  /** Exception or signal name. */
  std::string_view kind;
  /** Exception code or signal number. */
  u64 code = 0;
  /** Host program counter. */
  u64 pc = 0;
  /** Faulting data address, when the fault has one. */
  std::optional<u64> fault_address;
  /** Access direction of the faulting data address. */
  std::string_view access;
  /** Registers, or null when none were captured. */
  const HostThreadContext* context = nullptr;
};

/**
 * Takes the report guard.
 *
 * @return True for the first caller. Every later caller gets false.
 */
bool BeginReport();

/**
 * Reports whether InstallCrashReporter has run and UninstallCrashReporter has not.
 *
 * @return True when installed.
 */
bool IsCrashReporterInstalled();

/**
 * Writes the banner and every section of a fault report.
 *
 * @param fault The fault kind, addresses and context the platform handler captured.
 */
void ReportFault(const FaultReport& fault);

/**
 * Writes the banner, a message line and the backtrace.
 *
 * @param message The text written below the banner.
 */
void ReportMessage(std::string_view message);

/** Writes the closing line, flushes the log and shows the dialog when the options ask for it. */
void EndReport();

/**
 * Names the module that holds a host code address.
 *
 * @param address A host code address.
 * @return `module+0xRVA`, or an empty string when no module holds the address.
 */
std::string DescribeCode(u64 address);

/** Writes up to 32 frames of the calling thread. */
void LogBacktrace();

/**
 * Writes 1 backtrace line.
 *
 * @param index The frame index.
 * @param address The return address.
 */
void WriteBacktraceFrame(u32 index, u64 address);

/**
 * Installs the platform handlers.
 *
 * @return Ok, or a Platform error.
 */
Result<void> InstallPlatform();

/** Restores what InstallPlatform replaced. */
void UninstallPlatform();

#if !REX_PLATFORM_WIN32
/**
 * Writes a report for a fatal signal when the reporter is installed and no report has run.
 *
 * @param signal_number The fatal signal number.
 * @param info The signal info, or null.
 * @param signal_context The ucontext of the fault, or null.
 */
void ReportFatalSignal(int signal_number, siginfo_t* info, void* signal_context);

/**
 * Passes a signal no SDK handler claimed to the action it replaced. A default or ignored action, or
 * the same fault 64 times in a row, ends in a report and the default disposition.
 *
 * @param original The replaced action. Cleared to the default when it carries SA_RESETHAND.
 * @param signal_number The unclaimed signal number.
 * @param info The signal info.
 * @param signal_context The ucontext of the fault.
 */
void ForwardSignal(struct sigaction& original, int signal_number, siginfo_t* info,
                   void* signal_context);

/**
 * Ends the process through the default disposition of a signal.
 *
 * @param signal_number The signal number whose default disposition ends the process.
 */
[[noreturn]] void DieWithDefaultDisposition(int signal_number);
#endif

}  // namespace rex::platform::detail
