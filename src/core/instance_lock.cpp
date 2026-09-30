/**
 * @file        core/instance_lock.cpp
 * @brief       Named lock across processes
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <cstdlib>
#include <format>
#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace rex::platform {

#if REX_PLATFORM_WIN32
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
  const std::string full = std::format("Local\\rex-{}", name);
  const i32 wide_length =
      MultiByteToWideChar(CP_UTF8, 0, full.data(), i32(full.size()), nullptr, 0);
  std::wstring wide(size_t(wide_length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, full.data(), i32(full.size()), wide.data(), wide_length);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, wide.c_str());
  if (!mutex) {
    return Err<InstanceLock>(ErrorCategory::Platform,
                             std::format("CreateMutexW failed ({})", GetLastError()));
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return Err<InstanceLock>(ErrorCategory::Platform,
                             std::format("another instance holds '{}'", name));
  }
  auto handle = std::make_unique<detail::LockHandle>();
  handle->mutex = mutex;
  return InstanceLock(std::move(handle));
}
#else
struct detail::LockHandle {
  i32 fd = -1;
  ~LockHandle() {
    if (fd >= 0) {
      flock(fd, LOCK_UN);
      close(fd);
    }
  }
};

Result<InstanceLock> InstanceLock::Acquire(std::string_view name) {
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::string path =
      std::format("{}/rex-{}.lock", runtime_dir && *runtime_dir ? runtime_dir : "/tmp", name);
  const i32 fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Err<InstanceLock>(ErrorCategory::Platform, std::format("cannot open {}", path));
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return Err<InstanceLock>(ErrorCategory::Platform,
                             std::format("another instance holds '{}'", name));
  }
  auto handle = std::make_unique<detail::LockHandle>();
  handle->fd = fd;
  return InstanceLock(std::move(handle));
}
#endif

InstanceLock::InstanceLock(std::unique_ptr<detail::LockHandle> handle)
    : handle_(std::move(handle)) {}
InstanceLock::InstanceLock(InstanceLock&&) noexcept = default;
InstanceLock& InstanceLock::operator=(InstanceLock&&) noexcept = default;
InstanceLock::~InstanceLock() = default;

}  // namespace rex::platform
