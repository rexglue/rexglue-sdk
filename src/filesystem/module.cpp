#include "filesystem/module.h"

#include <cstring>

#include <rex/logging/log.h>
#include <rex/module.h>
#include <rex/result.h>

REXLOG_TABLE(kFileSystemLog, filesystem)

namespace rex::filesystem {

namespace {

FileSystemModule& GetInstance() {
  static FileSystemModule instance;
  return instance;
}

}  // namespace

void* FileSystemModule::GetModule() {
  return static_cast<IModule*>(&GetInstance());
}

void* FileSystemModule::GetFileSystem() {
  return static_cast<IFileSystem*>(&GetInstance().file_system_);
}

const char* FileSystemModule::GetName() const {
  return "rexfilesystem";
}

std::span<const char* const> FileSystemModule::GetRequiredInterfaces() const {
  return {};
}

Result<void> FileSystemModule::Connect(IInterfaceFactory* factory, const Config& config) {
  REX_TRY(logger_, factory->Get<ILogger>());
  REX_TRY_VOID(logger_->Register(log::kFileSystemLog));
  file_system_.SetConfig(&config.filesystem);
  return Ok();
}

void FileSystemModule::Disconnect() {
  file_system_.SetConfig(nullptr);
  if (logger_) {
    logger_->Unregister(log::kFileSystemLog);
  }
  logger_ = nullptr;
}

Result<void> FileSystemModule::Init() {
  file_system_.Clear();
  return Ok();
}

void FileSystemModule::Shutdown() {
  file_system_.Clear();
}

}  // namespace rex::filesystem

REX_DECLARE_MODULE(rex::filesystem::FileSystemModule)
