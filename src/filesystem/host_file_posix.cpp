#include "filesystem/host_file.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/chrono/chrono.h>
#include <rex/filesystem/error.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

u64 ToFileTime(time_t seconds) {
  return WinSystemClock::ToFileTime(
      WinSystemClock::from_sys(std::chrono::sys_seconds{std::chrono::seconds{seconds}}));
}

HostFileStat FromStat(std::filesystem::path name, const struct stat& st) {
  HostFileStat info;
  info.name = std::move(name);
  info.directory = S_ISDIR(st.st_mode);
  info.size = info.directory ? 0 : static_cast<u64>(st.st_size);
  info.create_time = ToFileTime(st.st_ctime);
  info.access_time = ToFileTime(st.st_atime);
  info.write_time = ToFileTime(st.st_mtime);
  info.device = static_cast<u64>(st.st_dev);
  info.inode = static_cast<u64>(st.st_ino);
  return info;
}

rex::Error HostError(std::string_view what, const std::filesystem::path& path) {
  return BuildError(FileSystemError::kIO, std::format("{} '{}' failed, {}", what, PathToUTF8(path),
                                                      std::strerror(errno)));
}

bool NoNativeExclusiveMove(int error) {
  return error == EINVAL || error == ENOSYS || error == ENOTSUP;
}

int MoveReplace(const std::filesystem::path& from, const std::filesystem::path& to) {
  return rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
}

int MoveNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__linux__)
  if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
    return 0;
  }
  if (!NoNativeExclusiveMove(errno)) {
    return errno;
  }
#elif defined(__APPLE__)
  if (renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) {
    return 0;
  }
  if (!NoNativeExclusiveMove(errno)) {
    return errno;
  }
#endif
  struct stat st{};
  if (lstat(from.c_str(), &st) != 0) {
    return errno;
  }
  if (!S_ISDIR(st.st_mode)) {
    if (link(from.c_str(), to.c_str()) == 0) {
      if (unlink(from.c_str()) == 0) {
        return 0;
      }
      const int error = errno;
      unlink(to.c_str());
      return error;
    }
    if (errno == EEXIST || !(errno == EPERM || errno == EMLINK || NoNativeExclusiveMove(errno))) {
      return errno;
    }
  }
  if (lstat(to.c_str(), &st) == 0) {
    return EEXIST;
  }
  return MoveReplace(from, to);
}

}  // namespace

HostFile::HostFile(Key /*key*/, std::filesystem::path path, NativeHandle native)
    : path_(std::move(path)), native_(native) {}

HostFile::~HostFile() {
  close(native_);
}

Result<std::unique_ptr<HostFile>> HostFile::OpenExisting(const std::filesystem::path& path,
                                                         Access access) {
  constexpr Access kReadRights = Access::kReadAccess | Access::kAllAccess | Access::kReadData;
  constexpr Access kWriteRights =
      Access::kWriteAccess | Access::kAllAccess | Access::kWriteData | Access::kAppendData;
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
    const int error = errno;
    FileSystemError code = FileSystemError::kIO;
    if (error == ENOENT || error == ENOTDIR) {
      code = FileSystemError::kNotFound;
    } else if (error == EACCES || error == EPERM || error == EROFS) {
      code = FileSystemError::kAccessDenied;
    } else if (error == ETXTBSY) {
      code = FileSystemError::kSharingViolation;
    }
    return Err<std::unique_ptr<HostFile>>(BuildError(
        code,
        std::format("cannot open host file '{}', {}", PathToUTF8(path), std::strerror(error))));
  }
  return std::make_unique<HostFile>(Key{}, path, handle);
}

Result<size_t> HostFile::Read(u64 offset, std::span<u8> buffer) {
  const ssize_t count = pread(native_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
  if (count < 0) {
    return Err<size_t>(HostError("read", path_));
  }
  return static_cast<size_t>(count);
}

Result<size_t> HostFile::Write(u64 offset, std::span<const u8> buffer) {
  const ssize_t count = pwrite(native_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
  if (count < 0) {
    return Err<size_t>(HostError("write", path_));
  }
  return static_cast<size_t>(count);
}

Result<void> HostFile::SetLength(u64 length) {
  if (ftruncate(native_, static_cast<off_t>(length)) < 0) {
    return Err(HostError("set length of", path_));
  }
  return Ok();
}

Result<void> HostFile::Flush() const {
  if (fsync(native_) < 0) {
    return Err(HostError("flush", path_));
  }
  return Ok();
}

[[nodiscard]] Result<void> HostCreateNew(const std::filesystem::path& path) {
  const int handle = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (handle == -1) {
    const int error = errno;
    FileSystemError code = FileSystemError::kIO;
    if (error == EEXIST) {
      code = FileSystemError::kExists;
    } else if (error == EACCES || error == EPERM || error == EROFS) {
      code = FileSystemError::kAccessDenied;
    }
    return Err(BuildError(code, std::format("cannot create host file '{}', {}", PathToUTF8(path),
                                            std::strerror(error))));
  }
  close(handle);
  return Ok();
}

[[nodiscard]] Result<void> HostMove(const std::filesystem::path& from,
                                    const std::filesystem::path& to, bool replace_existing) {
  const int error = replace_existing ? MoveReplace(from, to) : MoveNoReplace(from, to);
  if (error == 0) {
    return Ok();
  }
  FileSystemError code = FileSystemError::kIO;
  if (error == EEXIST || error == ENOTEMPTY) {
    code = FileSystemError::kExists;
  } else if (error == EACCES || error == EPERM || error == EROFS) {
    code = FileSystemError::kAccessDenied;
  } else if (error == EXDEV) {
    code = FileSystemError::kNotSameDevice;
  }
  return Err(BuildError(code, std::format("cannot move '{}' to '{}', {}", PathToUTF8(from),
                                          PathToUTF8(to), std::strerror(error))));
}

std::optional<HostFileStat> HostStat(const std::filesystem::path& path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) {
    return std::nullopt;
  }
  return FromStat(path.filename(), st);
}

std::vector<HostFileStat> HostList(const std::filesystem::path& path) {
  std::vector<HostFileStat> result;
  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }
  while (const dirent* entry = readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    struct stat st{};
    if (stat((path / entry->d_name).c_str(), &st) != 0) {
      continue;
    }
    result.push_back(FromStat(entry->d_name, st));
  }
  closedir(dir);
  return result;
}

}  // namespace rex::filesystem
