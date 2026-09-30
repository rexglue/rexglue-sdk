/**
 * @file        filesystem/host_file_posix.cpp
 * @brief       POSIX host file handle and host directory queries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/filesystem_posix.cc (Ben Vanik, 2020).
 */

#include "filesystem/host_file.h"

#include <cerrno>
#include <cstring>
#include <format>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/filesystem/error.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

constexpr u64 kUnixToFileTimeTicks = 116444736000000000ULL;
constexpr u64 kTicksPerSecond = 10000000ULL;

int Native(std::intptr_t native) {
  return static_cast<int>(native);
}

u64 ToFileTime(time_t seconds) {
  return (static_cast<u64>(seconds) * kTicksPerSecond) + kUnixToFileTimeTicks;
}

HostInfo FromStat(std::filesystem::path name, const struct stat& st) {
  HostInfo info;
  info.name = std::move(name);
  info.directory = S_ISDIR(st.st_mode);
  info.size = info.directory ? 0 : static_cast<u64>(st.st_size);
  info.create_time = ToFileTime(st.st_ctime);
  info.access_time = ToFileTime(st.st_atime);
  info.write_time = ToFileTime(st.st_mtime);
  return info;
}

rex::Error HostError(std::string_view what, const std::filesystem::path& path) {
  return MakeError(Error::kIO, std::format("{} '{}' failed, {}", what, path_to_utf8(path),
                                           std::strerror(errno)));
}

}  // namespace

HostFile::HostFile(std::filesystem::path path, std::intptr_t native)
    : path_(std::move(path)), native_(native) {}

HostFile::~HostFile() {
  close(Native(native_));
}

Result<std::unique_ptr<HostFile>> HostFile::OpenExisting(const std::filesystem::path& path,
                                                         Access access, bool share_delete) {
  (void)share_delete;
  constexpr Access kReadRights = Access::kGenericRead | Access::kGenericAll | Access::kReadData;
  constexpr Access kWriteRights =
      Access::kGenericWrite | Access::kGenericAll | Access::kWriteData | Access::kAppendData;
  const bool want_read = (access & kReadRights) != Access::kNone;
  const bool want_write = (access & kWriteRights) != Access::kNone;
  int mode = O_RDONLY;
  if (want_read && want_write) {
    mode = O_RDWR;
  } else if (want_write) {
    mode = O_WRONLY;
  }
  const int handle = open(path.c_str(), mode);
  if (handle == -1) {
    return Err<std::unique_ptr<HostFile>>(MakeError(
        Error::kNotFound,
        std::format("cannot open host file '{}', {}", path_to_utf8(path), std::strerror(errno))));
  }
  return std::unique_ptr<HostFile>(new HostFile(path, handle));
}

Result<size_t> HostFile::Read(u64 offset, std::span<u8> buffer) {
  const ssize_t count =
      pread(Native(native_), buffer.data(), buffer.size(), static_cast<off_t>(offset));
  if (count < 0) {
    return Err<size_t>(HostError("read", path_));
  }
  return static_cast<size_t>(count);
}

Result<size_t> HostFile::Write(u64 offset, std::span<const u8> buffer) {
  const ssize_t count =
      pwrite(Native(native_), buffer.data(), buffer.size(), static_cast<off_t>(offset));
  if (count < 0) {
    return Err<size_t>(HostError("write", path_));
  }
  return static_cast<size_t>(count);
}

Result<void> HostFile::SetLength(u64 length) {
  if (ftruncate(Native(native_), static_cast<off_t>(length)) < 0) {
    return Err(HostError("set length of", path_));
  }
  return Ok();
}

void HostFile::Flush() const {
  fsync(Native(native_));
}

std::optional<HostInfo> HostStat(const std::filesystem::path& path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) {
    return std::nullopt;
  }
  return FromStat(path.filename(), st);
}

std::vector<HostInfo> HostList(const std::filesystem::path& path) {
  std::vector<HostInfo> result;
  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }
  while (const dirent* entry = readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    struct stat st{};
    stat((path / entry->d_name).c_str(), &st);
    HostInfo info = FromStat(entry->d_name, st);
    info.directory = entry->d_type == DT_DIR;
    if (info.directory) {
      info.size = 0;
    }
    result.push_back(std::move(info));
  }
  closedir(dir);
  return result;
}

}  // namespace rex::filesystem
