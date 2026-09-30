/**
 * @file        filesystem/module.cpp
 * @brief       The rex.module@1 served by rexfilesystem.dll
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "filesystem/module.h"

#include <cstring>

#include <rex/macros.h>

namespace rex::filesystem {

namespace {

FilesystemModule& Instance() {
  static FilesystemModule instance;
  return instance;
}

}  // namespace

void* FilesystemModule::GetModule() {
  return static_cast<IModule*>(&Instance());
}

void* FilesystemModule::GetFileSystem() {
  return static_cast<IFileSystem*>(&Instance().file_system());
}

const char* FilesystemModule::Name() const {
  return "rexfilesystem";
}

std::span<const char* const> FilesystemModule::RequiredInterfaces() const {
  return {};
}

Result<void> FilesystemModule::Connect(IInterfaceFactory* factory, const Config& config) {
  (void)factory;
  file_system_.set_game_mount_flags(config.filesystem.game_mount_flags);
  return Ok();
}

void FilesystemModule::Disconnect() {}

Result<void> FilesystemModule::Init() {
  file_system_.Clear();
  return Ok();
}

void FilesystemModule::Shutdown() {
  file_system_.Clear();
}

}  // namespace rex::filesystem

REX_DECLARE_MODULE(rex::filesystem::FilesystemModule)
