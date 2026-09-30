/**
 * @file        filesystem/module.h
 * @brief       The rex.module@1 served by rexfilesystem.dll
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <span>

#include <rex/api.h>
#include <rex/filesystem/file_system.h>
#include <rex/module.h>

#include "filesystem/file_system.h"

namespace rex::filesystem {

/** The module rexfilesystem.dll serves. It owns the mount table. */
class FilesystemModule final : public IModule {
 public:
  /**
   * Returns the module instance for the rex.module@1 table entry.
   *
   * @return The IModule of rexfilesystem.
   */
  static void* GetModule();

  /**
   * Returns the mount table for the rex.filesystem@1 table entry.
   *
   * @return The IFileSystem of rexfilesystem.
   */
  static void* GetFileSystem();

  /** The interfaces rexfilesystem.dll serves. */
  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {IFileSystem::kInterfaceName, IFileSystem::kInterfaceVersion, &GetFileSystem},
  }};

  /**
   * Returns the module name the loader resolves to rexfilesystem.dll.
   *
   * @return "rexfilesystem".
   */
  const char* Name() const override;

  /**
   * Lists the interfaces Connect fetches.
   *
   * @return An empty list.
   */
  std::span<const char* const> RequiredInterfaces() const override;

  /**
   * Copies the filesystem slice.
   *
   * @param factory Unused.
   * @param config The configuration rex::Init was called with.
   * @return Ok.
   */
  Result<void> Connect(IInterfaceFactory* factory, const Config& config) override;

  /** Does nothing. */
  void Disconnect() override;

  /**
   * Empties the mount table.
   *
   * @return Ok.
   */
  Result<void> Init() override;

  /** Empties the mount table. */
  void Shutdown() override;

  /**
   * Returns the mount table.
   *
   * @return The table.
   */
  FileSystem& file_system() { return file_system_; }

 private:
  FileSystem file_system_;
};

}  // namespace rex::filesystem
