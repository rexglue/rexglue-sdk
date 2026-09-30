/**
 * @file        core/platform/file.cpp
 * @brief       Whole file helpers shared by every host
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/file.h>

#include <format>
#include <fstream>
#include <system_error>

#include <rex/string.h>

namespace rex::platform {

namespace {

Error IOError(std::string_view what, const std::filesystem::path& path) {
  return Error(ErrorCategory::IO, std::format("{} '{}'", what, path_to_utf8(path)));
}

}  // namespace

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

Result<std::vector<u8>> ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return Err<std::vector<u8>>(IOError("cannot open", path));
  }
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return Err<std::vector<u8>>(IOError("cannot size", path));
  }
  std::vector<u8> bytes(static_cast<size_t>(size));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
    return Err<std::vector<u8>>(IOError("cannot read", path));
  }
  return bytes;
}

Result<void> WriteWholeFile(const std::filesystem::path& path, std::span<const u8> data) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return Err(IOError("cannot create the folder of", path));
    }
  }
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Err(IOError("cannot open", temp));
    }
    if (!data.empty() && !stream.write(reinterpret_cast<const char*>(data.data()),
                                       static_cast<std::streamsize>(data.size()))) {
      return Err(IOError("cannot write", temp));
    }
    stream.close();
    if (!stream) {
      return Err(IOError("cannot flush", temp));
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return Err(IOError("cannot replace", path));
  }
  return Ok();
}

}  // namespace rex::platform
