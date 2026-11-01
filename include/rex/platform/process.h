/**
 * @file        platform/process.h
 * @brief       Process control, console, environment and crash reports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The debugger and message box parts are derived from Xenia's
 *              xenia/base/debugging.h (Ben Vanik, 2014) and xenia/base/system.h (Ben Vanik,
 *              2020).
 */

#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::platform {

namespace detail {
struct LockHandle;

/**
 * Builds the Windows command line that starts exe with args, each quoted so
 * CommandLineToArgvW returns it unchanged.
 *
 * @param exe Executable path, the first argument.
 * @param args Arguments after the executable, UTF-8.
 */
REX_API std::wstring BuildCommandLine(std::wstring_view exe, std::span<const std::string> args);
}  // namespace detail

/** A named lock across processes, held for the life of the object. */
class REX_API InstanceLock : public MoveOnly {
 public:
  InstanceLock(InstanceLock&& other) noexcept;
  InstanceLock& operator=(InstanceLock&& other) noexcept;
  ~InstanceLock();

  /**
   * Takes the lock.
   *
   * @param name Lock name, shared by every process of the app.
   * @return The held lock, or ErrorCategory::kPlatform when another process holds it.
   */
  [[nodiscard]] static Result<InstanceLock> Acquire(std::string_view name);

 private:
  explicit InstanceLock(std::unique_ptr<detail::LockHandle> handle);
  std::unique_ptr<detail::LockHandle> handle_;
};

/**
 * Starts this executable again as a new process and returns without waiting.
 *
 * @return Ok, or ErrorCategory::kPlatform with the OS error.
 */
[[nodiscard]] REX_API Result<void> Relaunch(std::span<const std::string> args);

/**
 * Starts an executable as a new process and returns without waiting.
 *
 * @param args Arguments after the executable.
 * @param working_directory The start directory of the new process, the current one when empty.
 * @return Ok, or ErrorCategory::kPlatform with the OS error.
 */
[[nodiscard]] REX_API Result<void> Relaunch(const std::filesystem::path& exe,
                                            std::span<const std::string> args,
                                            const std::filesystem::path& working_directory = {});

/** Reports whether the platform allows starting a new process. */
REX_API bool CanRelaunch();

struct OpenFileLimit {
  u64 before = 0; /**< Soft limit before the call, 0 where the host has none. */
  u64 after = 0;  /**< Soft limit after the call, 0 where the host has none. */
  u64 hard = 0;   /**< Hard limit, the largest u64 when unlimited, 0 where the host has none. */
};

/**
 * Raises the soft open file limit to the hard limit, or to 10240 when the hard limit is unlimited
 * or refused. Windows has no such limit, gives zeros and changes nothing.
 *
 * @return The limits, or ErrorCategory::kPlatform with the OS error.
 */
[[nodiscard]] REX_API Result<OpenFileLimit> RaiseOpenFileLimit();

/**
 * Describes the host resources this process uses, for logging when something runs out.
 *
 * @return The description of descriptors, memory maps or handles the host reports.
 */
[[nodiscard]] REX_API std::string DescribeResourceUse();

}  // namespace rex::platform

namespace rex::platform {

/**
 * Reports whether a stream is attached to a terminal.
 *
 * @param stream Stream to test, may be null.
 */
REX_API bool IsTerminal(FILE* stream);

/**
 * Turns on ANSI escape sequence handling for a stream. Does nothing on POSIX.
 *
 * @param stream Stream to configure, may be null.
 * @return True when escapes are handled on return, false for null or a console mode failure.
 */
REX_API bool EnableANSIEscapes(FILE* stream);

/** Switches the process console output code page to UTF-8. Does nothing on POSIX. */
REX_API void SetUTF8OutputCodePage();

}  // namespace rex::platform

namespace rex::platform {

/** Reports whether a debugger is attached. The check is expensive. */
REX_API bool IsDebuggerAttached();

/** Breaks into the attached debugger. Without one, POSIX raises SIGTRAP. */
REX_API void Break();

}  // namespace rex::platform

namespace rex::platform {

/**
 * Reads an environment variable.
 *
 * @param name Variable name, UTF-8.
 * @return The UTF-8 value, or std::nullopt when not set. An empty value is not std::nullopt.
 */
REX_API std::optional<std::string> GetEnvironmentValue(std::string_view name);

/**
 * Sets an environment variable, replacing any existing value.
 *
 * @param name Variable name, UTF-8.
 * @param value New value, UTF-8.
 */
REX_API bool SetEnvironmentValue(std::string_view name, std::string_view value);

/**
 * Removes an environment variable.
 *
 * @param name Variable name, UTF-8.
 * @return True when the OS call succeeds.
 */
REX_API bool UnsetEnvironmentValue(std::string_view name);

/**
 * Lists every environment variable of the process.
 *
 * @return Name and value pairs in UTF-8.
 */
REX_API std::vector<std::pair<std::string, std::string>> EnumerateEnvironment();

}  // namespace rex::platform

namespace rex {

/** Severity of a message box, which picks its icon. */
enum class MessageType : u8 {
  kInfo,
  kWarning,
  kError,
};

/**
 * Shows a native message box and blocks until it closes. On POSIX it writes to stderr instead.
 *
 * @param type The icon and severity.
 * @param message The UTF-8 text.
 */
REX_API void ShowMessageBox(MessageType type, std::string_view message);

}  // namespace rex

namespace rex::platform {

struct CrashReporterOptions {
  /** Name in the report and dialog. Empty uses the executable name. */
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::string app_name = {};
  bool show_dialog = false;
};

/**
 * Describes a host fault address in title terms. Runs in a signal handler, so it must not allocate
 * or lock.
 *
 * @param host_address The fault address, or 0 when there is none.
 * @param out Buffer for the text.
 * @return The text length, 0 for none.
 */
using CrashAddressDescriber = u32 (*)(u64 host_address, std::span<char> out);

/**
 * Installs the crash reporter, or replaces its options when installed. Prepares the calling thread.
 *
 * @return Ok, or a Platform error when a handler cannot be installed.
 */
[[nodiscard]] REX_API Result<void> InstallCrashReporter(const CrashReporterOptions& options);

/** Removes the crash reporter and restores the handlers it replaced. */
REX_API void UninstallCrashReporter();

/**
 * Sets the function the report calls to describe the fault address.
 *
 * @param describer The describer, or null for none.
 */
REX_API void SetCrashAddressDescriber(CrashAddressDescriber describer);

/** Reserves the stack a report needs on the calling thread, so stack overflow reports work. */
REX_API void PrepareThreadForCrash();

}  // namespace rex::platform
