/**
 * @file        platform/process.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::platform {

namespace detail {
struct LockHandle;

/**
 * Builds the Windows command line that starts exe with args, each quoted so
 * CommandLineToArgvW returns it unchanged.
 *
 * @param exe Executable path, the first argument.
 * @param args Arguments after the executable, UTF-8.
 * @return The command line.
 */
REX_API std::wstring BuildCommandLine(std::wstring_view exe, std::span<const std::string> args);
}  // namespace detail

/** A named lock across processes, held for the life of the object. */
class REX_API InstanceLock : public MoveOnly {
 public:
  /**
   * Takes ownership of the lock held by another object.
   *
   * @param other The object to take from, left empty.
   */
  InstanceLock(InstanceLock&& other) noexcept;
  /**
   * Releases the lock held, then takes ownership of the lock held by another object.
   *
   * @param other The object to take from, left empty.
   * @return This object.
   */
  InstanceLock& operator=(InstanceLock&& other) noexcept;
  /** Releases the lock. */
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
 * @param args Arguments for the new process.
 * @return Ok, or ErrorCategory::kPlatform with the OS error.
 */
[[nodiscard]] REX_API Result<void> Relaunch(std::span<const std::string> args);

/**
 * Starts an executable as a new process and returns without waiting. It fails when the executable
 * cannot be started or the directory does not exist.
 *
 * @param exe The executable to launch.
 * @param args Arguments after the executable.
 * @param working_directory The start directory of the new process, the current one when empty.
 * @return Ok, or ErrorCategory::kPlatform with the OS error.
 */
[[nodiscard]] REX_API Result<void> Relaunch(const std::filesystem::path& exe,
                                            std::span<const std::string> args,
                                            const std::filesystem::path& working_directory = {});

/**
 * Reports whether this process may start a new one.
 *
 * @return True when the platform allows starting a new process.
 */
REX_API bool CanRelaunch();

/** The open file limit of the process. */
struct OpenFileLimit {
  u64 before = 0; /**< The soft limit before the call, 0 where the host has no such limit. */
  u64 after = 0;  /**< The soft limit after the call, 0 where the host has no such limit. */
  u64 hard = 0;   /**< The hard limit, the largest u64 when unlimited, 0 where there is no limit. */
};

/**
 * Raises the soft open file limit of the process to its hard limit, or to 10240 when the hard limit
 * is unlimited or the host refuses the hard limit. Child processes inherit the new limit. A host
 * without a per process limit, Windows, gives zeros and changes nothing.
 *
 * @return The limits, or ErrorCategory::kPlatform with the OS error when the limit cannot be read
 *         or raised.
 */
[[nodiscard]] REX_API Result<OpenFileLimit> RaiseOpenFileLimit();

/**
 * Describes how much of the host resources this process uses, for logging when something runs out.
 *
 * @return On Linux the open descriptors and memory maps with their limits, on macOS the open
 *         descriptors and memory regions, on Windows the handle count.
 */
[[nodiscard]] REX_API std::string DescribeResourceUse();

}  // namespace rex::platform
