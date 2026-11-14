/**
 * @file        base/instance_lock_posix.cpp
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
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace rex::platform {

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
  const std::string runtime_dir = GetEnvironmentValue("XDG_RUNTIME_DIR").value_or("");
  const std::string_view directory = runtime_dir.empty() ? std::string_view("/tmp") : runtime_dir;
  const std::string path = std::format("{}/rex-{}.lock", directory, name);
  const i32 fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Err<InstanceLock>(ErrorCategory::kPlatform, std::format("cannot open {}", path));
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return Err<InstanceLock>(ErrorCategory::kPlatform,
                             std::format("another instance holds '{}'", name));
  }
  auto handle = std::make_unique<detail::LockHandle>();
  handle->fd = fd;
  return InstanceLock(std::move(handle));
}

InstanceLock::InstanceLock(std::unique_ptr<detail::LockHandle> handle)
    : handle_(std::move(handle)) {}
InstanceLock::InstanceLock(InstanceLock&&) noexcept = default;
InstanceLock& InstanceLock::operator=(InstanceLock&&) noexcept = default;
InstanceLock::~InstanceLock() = default;

}  // namespace rex::platform
