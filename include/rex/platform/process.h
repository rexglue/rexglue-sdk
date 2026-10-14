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

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <rex/macros.h>
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
  InstanceLock(InstanceLock&&) noexcept;
  /**
   * Releases the lock held, then takes ownership of the lock held by another object.
   *
   * @param other The object to take from, left empty.
   * @return This object.
   */
  InstanceLock& operator=(InstanceLock&&) noexcept;
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
 * Reports whether this process may start a new one.
 *
 * @return False where the platform forbids it, such as Steam Game Mode.
 */
REX_API bool CanRelaunch();

}  // namespace rex::platform
