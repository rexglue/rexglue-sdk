#include "filesystem/host_file.h"

#include <cwchar>
#include <format>

#include <windows.h>

#include <rex/filesystem/error.h>
#include <rex/logging.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

HANDLE Native(std::intptr_t native) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<HANDLE>(native);
}

u64 Combine(FILETIME time) {
  return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

u64 Combine(DWORD high, DWORD low) {
  return (static_cast<u64>(high) << 32) | low;
}

OVERLAPPED At(u64 offset) {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  return overlapped;
}

rex::Error HostError(std::string_view what, const std::filesystem::path& path) {
  return MakeError(Error::kIO, std::format("{} '{}' failed, error {}", what, path_to_utf8(path),
                                           GetLastError()));
}

}  // namespace

HostFile::HostFile(Key /*key*/, std::filesystem::path path, std::intptr_t native)
    : path_(std::move(path)), native_(native) {}

HostFile::~HostFile() {
  CloseHandle(Native(native_));
}

Result<std::unique_ptr<HostFile>> HostFile::OpenExisting(const std::filesystem::path& path,
                                                         Access access) {
  DWORD open_access = 0;
  if (HasFlags(access, Access::kGenericRead)) {
    open_access |= GENERIC_READ;
  }
  if (HasFlags(access, Access::kGenericWrite)) {
    open_access |= GENERIC_WRITE;
  }
  if (HasFlags(access, Access::kGenericExecute)) {
    open_access |= GENERIC_EXECUTE;
  }
  if (HasFlags(access, Access::kGenericAll)) {
    open_access |= GENERIC_READ | GENERIC_WRITE;
  }
  if (HasFlags(access, Access::kReadData)) {
    open_access |= FILE_READ_DATA;
  }
  if (HasFlags(access, Access::kWriteData)) {
    open_access |= FILE_WRITE_DATA;
  }
  if (HasFlags(access, Access::kAppendData)) {
    open_access |= FILE_APPEND_DATA;
  }
  constexpr DWORD kShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE handle = CreateFileW(path.c_str(), open_access, kShareMode, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    Error code = Error::kIO;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      code = Error::kNotFound;
    } else if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      code = Error::kSharingViolation;
    } else if (error == ERROR_ACCESS_DENIED) {
      code = Error::kAccessDenied;
    }
    return Err<std::unique_ptr<HostFile>>(MakeError(
        code, std::format("cannot open host file '{}', error {}", path_to_utf8(path), error)));
  }
  return std::make_unique<HostFile>(Key{}, path, reinterpret_cast<std::intptr_t>(handle));
}

Result<size_t> HostFile::Read(u64 offset, std::span<u8> buffer) {
  OVERLAPPED overlapped = At(offset);
  DWORD bytes_read = 0;
  if (!ReadFile(Native(native_), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read,
                &overlapped)) {
    if (GetLastError() == ERROR_HANDLE_EOF) {
      return size_t{0};
    }
    if (GetLastError() == ERROR_NOACCESS) {
      REXLOG_WARN(filesystem, "host read of {:#x} bytes into {} was refused, read only memory",
                  buffer.size(), static_cast<const void*>(buffer.data()));
    }
    return Err<size_t>(HostError("read", path_));
  }
  return static_cast<size_t>(bytes_read);
}

Result<size_t> HostFile::Write(u64 offset, std::span<const u8> buffer) {
  OVERLAPPED overlapped = At(offset);
  DWORD bytes_written = 0;
  if (!WriteFile(Native(native_), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_written,
                 &overlapped)) {
    return Err<size_t>(HostError("write", path_));
  }
  return static_cast<size_t>(bytes_written);
}

Result<void> HostFile::SetLength(u64 length) {
  FILE_END_OF_FILE_INFO info{};
  info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
  if (!SetFileInformationByHandle(Native(native_), FileEndOfFileInfo, &info, sizeof(info))) {
    return Err(HostError("set length of", path_));
  }
  return Ok();
}

Result<void> HostFile::Flush() const {
  if (!FlushFileBuffers(Native(native_))) {
    return Err(HostError("flush", path_));
  }
  return Ok();
}

[[nodiscard]] Result<void> HostCreateNew(const std::filesystem::path& path) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    Error code = Error::kIO;
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      code = Error::kExists;
    } else if (error == ERROR_ACCESS_DENIED) {
      code = Error::kAccessDenied;
    }
    return Err(MakeError(
        code, std::format("cannot create host file '{}', error {}", path_to_utf8(path), error)));
  }
  CloseHandle(handle);
  return Ok();
}

[[nodiscard]] Result<void> HostMove(const std::filesystem::path& from,
                                    const std::filesystem::path& to, bool replace_existing) {
  if (MoveFileExW(from.c_str(), to.c_str(), replace_existing ? MOVEFILE_REPLACE_EXISTING : 0)) {
    return Ok();
  }
  const DWORD error = GetLastError();
  Error code = Error::kIO;
  if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
    code = Error::kExists;
  } else if (error == ERROR_SHARING_VIOLATION) {
    code = Error::kSharingViolation;
  } else if (error == ERROR_ACCESS_DENIED) {
    code = Error::kAccessDenied;
  }
  return Err(MakeError(code, std::format("cannot move '{}' to '{}', error {}", path_to_utf8(from),
                                         path_to_utf8(to), error)));
}

std::optional<HostInfo> HostStat(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return std::nullopt;
  }
  HostInfo info;
  info.name = path.filename();
  info.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  info.size = info.directory ? 0 : Combine(data.nFileSizeHigh, data.nFileSizeLow);
  info.create_time = Combine(data.ftCreationTime);
  info.access_time = Combine(data.ftLastAccessTime);
  info.write_time = Combine(data.ftLastWriteTime);
  return info;
}

std::vector<HostInfo> HostList(const std::filesystem::path& path) {
  std::vector<HostInfo> result;
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW((path / "*").c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    return result;
  }
  do {
    if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0) {
      continue;
    }
    HostInfo info;
    info.name = data.cFileName;
    info.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = info.directory ? 0 : Combine(data.nFileSizeHigh, data.nFileSizeLow);
    info.create_time = Combine(data.ftCreationTime);
    info.access_time = Combine(data.ftLastAccessTime);
    info.write_time = Combine(data.ftLastWriteTime);
    result.push_back(std::move(info));
  } while (FindNextFileW(handle, &data) != 0);
  FindClose(handle);
  return result;
}

}  // namespace rex::filesystem
