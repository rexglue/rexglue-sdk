/**
 * @file        tests/unit/filesystem/fixture.h
 * @brief       Catch2 fixture that loads rex-filesystem through rex::Init
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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/platform/file.h>
#include <rex/types.h>

#include "support/temp_folder.h"

namespace rex::filesystem::test {

/** Host folder removed with its contents on destruction. */
struct TempFolder : rex::test::TempFolder {
  TempFolder() : rex::test::TempFolder("rex_filesystem_test_") {}
};

/**
 * Writes text into a host file, creating parent folders.
 *
 * @param path The host file.
 * @param text The contents.
 */
inline void WriteHost(const std::filesystem::path& path, std::string_view text) {
  REQUIRE(platform::WriteWholeFile(
      path, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}

/**
 * Reads every byte of an open file.
 *
 * @param file The open file.
 * @return The contents as text.
 */
inline std::string ReadText(IFile& file) {
  std::vector<u8> bytes(file.GetInfo().size);
  if (bytes.empty()) {
    return {};
  }
  auto read = file.Read(bytes, 0);
  REQUIRE(read);
  return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(*read)};
}

/** Loads rex-filesystem, holds its IFileSystem and a temp folder, and shuts down. */
class Fixture : public NonCopyable {
 public:
  /**
   * Runs rex::Init with rex-filesystem as the only module.
   *
   * @param game_mount_flags The filesystem mount flags handed to Connect.
   */
  explicit Fixture(MountFlags game_mount_flags = MountFlags::kNone) {
    Config config;
    config.modules = {"rex-filesystem"};
    config.filesystem.game_mount_flags = game_mount_flags;
    auto api = rex::Init(config);
    REQUIRE(api);
    file_system_ = (*api)->Get<IFileSystem>();
    if (!file_system_) {
      rex::Shutdown();
    }
    REQUIRE(file_system_);
  }

  ~Fixture() { rex::Shutdown(); }

  /**
   * Returns the mount table the module serves.
   *
   * @return The interface.
   */
  IFileSystem& GetFs() { return *file_system_; }

  /**
   * Returns the temp folder of the case.
   *
   * @return The host folder.
   */
  const std::filesystem::path& GetTemp() const { return temp_.path; }

  /**
   * Mounts the temp folder at a path as a host directory.
   *
   * @param path The guest mount path.
   * @param flags The mount flags.
   */
  void MountTemp(std::string_view path, MountFlags flags = MountFlags::kWritable) {
    REQUIRE(GetFs().Mount({std::string(path), ProviderKind::kHostDirectory, GetTemp(), flags}));
  }

  /**
   * Opens a path and requires success.
   *
   * @param path A guest path.
   * @param options Disposition, access and kind.
   * @return The opened file and action.
   */
  Opened Open(std::string_view path, const OpenOptions& options = {}) {
    auto opened = GetFs().Open(path, options);
    REQUIRE(opened);
    return std::move(*opened);
  }

 private:
  TempFolder temp_;
  IFileSystem* file_system_ = nullptr;
};

}  // namespace rex::filesystem::test
