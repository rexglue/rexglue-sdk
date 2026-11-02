/**
 * @file        tests/unit/filesystem/fixture.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/file_open.h>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/file_system.h>
#include <rex/platform/file.h>
#include <rex/types.h>
#include <rex/utility.h>

#include "filesystem/file_system.h"
#include "temp_folder.h"

namespace rex::filesystem::test {

/** Host folder removed with its contents on destruction. */
struct TempFolder : rex::test::TempFolder {
  TempFolder() : rex::test::TempFolder("rex_filesystem_test_") {}
};

/** Writes text into a host file, creating parent folders. */
inline void WriteHost(const std::filesystem::path& path, std::string_view text) {
  REQUIRE(platform::WriteHostFile(
      path, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}

/** Returns the whole contents of an open file as text. */
inline std::string ReadText(IFile& file) {
  std::vector<u8> bytes(file.GetInfo().size);
  if (bytes.empty()) {
    return {};
  }
  auto read = file.Read(bytes, 0);
  REQUIRE(read);
  return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(*read)};
}

class Fixture : public NonCopyable {
 public:
  Fixture() : file_system_(std::make_unique<FileSystem>()) {}

  IFileSystem& GetFileSystem() { return *file_system_; }

  const std::filesystem::path& GetTemp() const { return temp_.path; }

  void MountTemp(std::string_view path, MountFlags flags = MountFlags::kWritable) {
    REQUIRE(
        GetFileSystem().Mount({std::string(path), ProviderType::kHostDirectory, GetTemp(), flags}));
  }

  /** Opens a guest path and requires success. */
  Opened Open(std::string_view path, const OpenOptions& options = {}) {
    auto opened = GetFileSystem().Open(path, options);
    REQUIRE(opened);
    return std::move(*opened);
  }

 private:
  TempFolder temp_;
  std::unique_ptr<FileSystem> file_system_;
};

}  // namespace rex::filesystem::test
