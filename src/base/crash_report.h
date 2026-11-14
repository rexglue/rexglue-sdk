/**
 * @file        base/crash_report.h
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
#include <span>
#include <string>
#include <string_view>

#include <rex/platform/exception_handler.h>
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

/** Takes the report guard. */
bool BeginReport();

/**
 * Blocks a thread that faults while another thread holds the report guard, until the process
 * ends. Returns at once on the thread that holds the guard.
 */
void WaitForOtherReport();

/** Reports whether InstallCrashReporter has run and UninstallCrashReporter has not. */
bool IsCrashReporterInstalled();

/** Writes the banner and every section of a fault report. */
void ReportFault(const FaultReport& fault);

/** Writes the banner, a message line and the backtrace. */
void ReportMessage(std::string_view message);

/** Writes the closing line, flushes the log and shows the dialog when the options ask for it. */
void EndReport();

/**
 * Names the module that holds a host code address, without allocating or locking.
 *
 * @param out The buffer the name goes into.
 * @return `module+0xRVA` inside out, or an empty view when no module holds the address.
 */
std::string_view DescribeCode(u64 address, std::span<char> out);

/**
 * Reports whether a host range can be read without a fault.
 *
 * @param address The first byte.
 * @param size The byte count.
 * @return True when every byte is readable.
 */
bool IsReadable(u64 address, u64 size);

/** Writes up to 32 frames of the calling thread. */
void LogBacktrace();

void WriteBacktraceFrame(u32 index, u64 address);

/**
 * @return Ok, or a Platform error.
 */
[[nodiscard]] Result<void> InstallPlatform();

/** Restores what InstallPlatform replaced. */
void UninstallPlatform();

#if !REX_PLATFORM_WIN32
/** Sends every later report line through WriteRaw instead of the logger. */
void UseRawOutput();

/**
 * Writes a report line to stderr and the log file without allocating or locking.
 *
 * @param text The line, without its newline.
 */
void WriteRaw(std::string_view text);
#endif

}  // namespace rex::platform::detail
