/**
 * @file        base/instance_lock_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <format>
#include <string>

#include <windows.h>

#include <rex/string/text.h>

namespace rex::platform {

struct detail::LockHandle {
  HANDLE mutex = nullptr;
  ~LockHandle() {
    if (mutex) {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
    }
  }
};

Result<InstanceLock> InstanceLock::Acquire(std::string_view name) {
  const std::u16string wide = rex::string::ToUTF16(std::format("Local\\rex-{}", name));
  HANDLE mutex = CreateMutexW(nullptr, TRUE, reinterpret_cast<LPCWSTR>(wide.c_str()));
  if (!mutex) {
    return Err<InstanceLock>(ErrorCategory::kPlatform,
                             std::format("CreateMutexW failed ({})", GetLastError()));
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return Err<InstanceLock>(ErrorCategory::kPlatform,
                             std::format("another instance holds '{}'", name));
  }
  auto handle = std::make_unique<detail::LockHandle>();
  handle->mutex = mutex;
  return InstanceLock(std::move(handle));
}

InstanceLock::InstanceLock(std::unique_ptr<detail::LockHandle> handle)
    : handle_(std::move(handle)) {}
InstanceLock::InstanceLock(InstanceLock&&) noexcept = default;
InstanceLock& InstanceLock::operator=(InstanceLock&&) noexcept = default;
InstanceLock::~InstanceLock() = default;

}  // namespace rex::platform
