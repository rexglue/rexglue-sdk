/**
 * @file        filesystem/module.h
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
#include <rex/logging/logger.h>
#include <rex/module.h>

#include "filesystem/file_system.h"

namespace rex::filesystem {

/** The module rexfilesystem.dll serves. It owns the mount table. */
class FilesystemModule final : public IModule {
 public:
  /**
   * Returns the module instance for its interface table entry.
   *
   * @return The IModule of rexfilesystem.
   */
  static void* GetModule();

  /**
   * Returns the mount table for its interface table entry.
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
  const char* GetName() const override;

  /**
   * Lists the interfaces Connect fetches.
   *
   * @return An empty list.
   */
  std::span<const char* const> GetRequiredInterfaces() const override;

  /**
   * Copies FilesystemConfig and registers the filesystem log category.
   *
   * @param factory Serves the logger.
   * @param config The configuration rex::Init was called with.
   * @return Ok, or the registration error.
   */
  [[nodiscard]] Result<void> Connect(IInterfaceFactory* factory, const Config& config) override;

  /** Unregisters the log category. */
  void Disconnect() override;

  /**
   * Empties the mount table.
   *
   * @return Ok.
   */
  [[nodiscard]] Result<void> Init() override;

  /** Empties the mount table. */
  void Shutdown() override;

 private:
  ILogger* logger_ = nullptr;
  FileSystem file_system_;
};

}  // namespace rex::filesystem
