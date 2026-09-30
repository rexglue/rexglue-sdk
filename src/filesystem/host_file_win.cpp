/**
 * @file        filesystem/host_file_win.cpp
 * @brief       Windows host file handle and host directory queries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/filesystem_win.cc (Ben Vanik, 2020).
 */

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

HostFile::HostFile(std::filesystem::path path, std::intptr_t native)
    : path_(std::move(path)), native_(native) {}

HostFile::~HostFile() {
  CloseHandle(Native(native_));
}

Result<std::unique_ptr<HostFile>> HostFile::OpenExisting(const std::filesystem::path& path,
                                                         Access access, bool share_delete) {
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
  DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
  if (share_delete) {
    share_mode |= FILE_SHARE_DELETE;
  }
  HANDLE handle = CreateFileW(path.c_str(), open_access, share_mode, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Err<std::unique_ptr<HostFile>>(MakeError(
        Error::kNotFound,
        std::format("cannot open host file '{}', error {}", path_to_utf8(path), GetLastError())));
  }
  return std::unique_ptr<HostFile>(new HostFile(path, reinterpret_cast<std::intptr_t>(handle)));
}

Result<size_t> HostFile::Read(u64 offset, std::span<u8> buffer) {
  OVERLAPPED overlapped = At(offset);
  DWORD bytes_read = 0;
  if (!ReadFile(Native(native_), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read,
                &overlapped)) {
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
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(length);
  if (!SetFilePointerEx(Native(native_), position, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(Native(native_))) {
    return Err(HostError("set length of", path_));
  }
  return Ok();
}

void HostFile::Flush() const {
  FlushFileBuffers(Native(native_));
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
