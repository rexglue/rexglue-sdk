/**
 * @file        filesystem/module.h
 * @brief       The module interface served by rex-filesystem.dll
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

/** The module rex-filesystem.dll serves. It owns the mount table. */
class FilesystemModule final : public IModule {
 public:
  /**
   * Returns the module instance for its interface table entry.
   *
   * @return The IModule of rex-filesystem.
   */
  static void* GetModule();

  /**
   * Returns the mount table for its interface table entry.
   *
   * @return The IFileSystem of rex-filesystem.
   */
  static void* GetFileSystem();

  /** The interfaces rex-filesystem.dll serves. */
  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {IFileSystem::kInterfaceName, IFileSystem::kInterfaceVersion, &GetFileSystem},
  }};

  /**
   * Returns the module name the loader resolves to rex-filesystem.dll.
   *
   * @return "rex-filesystem".
   */
  const char* Name() const override;

  /**
   * Lists the interfaces Connect fetches.
   *
   * @return An empty list.
   */
  std::span<const char* const> RequiredInterfaces() const override;

  /**
   * Copies the filesystem slice and registers the filesystem log category.
   *
   * @param factory Serves the logger.
   * @param config The configuration rex::Init was called with.
   * @return Ok, or the registration error.
   */
  Result<void> Connect(IInterfaceFactory* factory, const Config& config) override;

  /** Unregisters the log category. */
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
  ILogger* logger_ = nullptr;
  FileSystem file_system_;
};

}  // namespace rex::filesystem
