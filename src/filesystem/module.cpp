#include "filesystem/module.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/macros.h>

REXLOG_TABLE(kFilesystemLog, filesystem)

namespace rex::filesystem {

namespace {

FilesystemModule& GetInstance() {
  static FilesystemModule instance;
  return instance;
}

}  // namespace

void* FilesystemModule::GetModule() {
  return static_cast<IModule*>(&GetInstance());
}

void* FilesystemModule::GetFileSystem() {
  return static_cast<IFileSystem*>(&GetInstance().file_system_);
}

const char* FilesystemModule::GetName() const {
  return "rexfilesystem";
}

std::span<const char* const> FilesystemModule::GetRequiredInterfaces() const {
  return {};
}

Result<void> FilesystemModule::Connect(IInterfaceFactory* factory, const Config& config) {
  REX_TRY(logger_, factory->Get<ILogger>());
  REX_TRY_VOID(logger_->Register(log::kFilesystemLog));
  file_system_.SetGameMountFlags(config.filesystem.game_mount_flags);
  return Ok();
}

void FilesystemModule::Disconnect() {
  if (logger_) {
    logger_->Unregister(log::kFilesystemLog);
  }
  logger_ = nullptr;
}

Result<void> FilesystemModule::Init() {
  file_system_.Clear();
  return Ok();
}

void FilesystemModule::Shutdown() {
  file_system_.Clear();
}

}  // namespace rex::filesystem

REX_DECLARE_MODULE(rex::filesystem::FilesystemModule)
