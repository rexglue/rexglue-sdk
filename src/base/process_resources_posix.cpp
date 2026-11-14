/**
 * @file        base/process_resources_posix.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <string>

#include <dirent.h>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif

namespace rex::platform {

namespace {

constexpr rlim_t kUnlimitedTarget = 10240;

u64 ToU64(rlim_t value) {
  return value == RLIM_INFINITY ? std::numeric_limits<u64>::max() : static_cast<u64>(value);
}

long CountOpenDescriptors() {
#if defined(__APPLE__)
  DIR* dir = ::opendir("/dev/fd");
#else
  DIR* dir = ::opendir("/proc/self/fd");
#endif
  if (!dir) {
    return -1;
  }
  long count = 0;
  while (const dirent* entry = ::readdir(dir)) {
    if (entry->d_name[0] != '.') {
      ++count;
    }
  }
  ::closedir(dir);
  return count - 1;
}

#if defined(__APPLE__)

long CountMemoryRegions() {
  mach_vm_address_t address = 0;
  mach_vm_size_t size = 0;
  long regions = 0;
  vm_region_basic_info_data_64_t info;
  while (true) {
    mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    const kern_return_t status =
        mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
    if (status != KERN_SUCCESS) {
      break;
    }
    if (object_name != MACH_PORT_NULL) {
      ::mach_port_deallocate(mach_task_self(), object_name);
    }
    ++regions;
    if (size == 0) {
      break;
    }
    address += size;
  }
  return regions;
}

#else

long CountLines(const char* path) {
  FILE* file = ::fopen(path, "re");
  if (!file) {
    return -1;
  }
  long lines = 0;
  for (int c = ::fgetc(file); c != EOF; c = ::fgetc(file)) {
    if (c == '\n') {
      ++lines;
    }
  }
  ::fclose(file);
  return lines;
}

long ReadLong(const char* path) {
  FILE* file = ::fopen(path, "re");
  if (!file) {
    return -1;
  }
  char line[64];
  const char* const text = std::fgets(line, sizeof(line), file);
  ::fclose(file);
  if (!text) {
    return -1;
  }
  const char* begin = text;
  const char* const end = text + std::strlen(text);
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  long value = -1;
  if (std::from_chars(begin, end, value).ec != std::errc{}) {
    return -1;
  }
  return value;
}

#endif

}  // namespace

Result<OpenFileLimit> RaiseOpenFileLimit() {
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return Err<OpenFileLimit>(ErrorCategory::kPlatform,
                              std::format("getrlimit failed, {}", std::strerror(errno)), errno);
  }
  OpenFileLimit result{ToU64(limit.rlim_cur), ToU64(limit.rlim_cur), ToU64(limit.rlim_max)};
  const rlim_t target = limit.rlim_max == RLIM_INFINITY ? kUnlimitedTarget : limit.rlim_max;
  if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= target) {
    return result;
  }
  const rlim_t current = limit.rlim_cur;
  const auto apply = [&limit](rlim_t value) {
    limit.rlim_cur = value;
    return ::setrlimit(RLIMIT_NOFILE, &limit) == 0;
  };
  rlim_t chosen = target;
  if (!apply(target)) {
    if (target <= kUnlimitedTarget || kUnlimitedTarget <= current || !apply(kUnlimitedTarget)) {
      return Err<OpenFileLimit>(ErrorCategory::kPlatform,
                                std::format("setrlimit failed, {}", std::strerror(errno)), errno);
    }
    chosen = kUnlimitedTarget;
  }
  result.after = ToU64(chosen);
  return result;
}

std::string DescribeResourceUse() {
  rlimit limit{};
  const long descriptor_limit =
      ::getrlimit(RLIMIT_NOFILE, &limit) == 0 ? static_cast<long>(limit.rlim_cur) : -1;
#if defined(__APPLE__)
  return std::format("fds {}/{}, maps {}/-1", CountOpenDescriptors(), descriptor_limit,
                     CountMemoryRegions());
#else
  return std::format("fds {}/{}, maps {}/{}", CountOpenDescriptors(), descriptor_limit,
                     CountLines("/proc/self/maps"), ReadLong("/proc/sys/vm/max_map_count"));
#endif
}

}  // namespace rex::platform
