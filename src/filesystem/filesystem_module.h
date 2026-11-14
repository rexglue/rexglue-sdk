#pragma once

#include <memory>
#include <span>
#include <string_view>

#include <rex/filesystem/file_system.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/memory/host.h>
#include <rex/module.h>

#include "filesystem/container.h"
#include "filesystem/file_system.h"
#include "filesystem/xdbf_title_data.h"
#include "filesystem/xex_image.h"
#include <rex/kernel/types/xex.h>

REXLOG_TABLE(kFileSystemLog, filesystem)

namespace rex::filesystem {

class FileSystemModule final : public IModule, public IFileSystemFactory {
 public:
  REX_MODULE_SERVES(::rex::filesystem::IFileSystemFactory)

  std::string_view GetName() const override { return "rexfilesystem"; }

  std::span<const InterfaceType> GetRequiredInterfaces() const override {
    static constexpr InterfaceType kRequired[] = {ILogger::kInterface};
    return kRequired;
  }

  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override {
    memory::SetTagName(kFilesystem, "Filesystem");
    logger_ = factory.Get<ILogger>();
    return logger_->AddCategories(log::kFileSystemLog);
  }

  void Disconnect() override {
    logger_->RemoveCategories(log::kFileSystemLog);
    logger_ = nullptr;
  }

  [[nodiscard]] Result<void> Init() override { return {}; }

  void Shutdown() override {}

  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IFileSystemFactory>(*this, name, version);
  }

  std::unique_ptr<IFileSystem> Create() override { return std::make_unique<FileSystem>(); }

  Result<std::unique_ptr<IXEXImage>> ParseXEX(std::span<const u8> file) override {
    auto image = XEXImage::Parse(file);
    if (!image) {
      return Err<std::unique_ptr<IXEXImage>>(image.error());
    }
    return std::unique_ptr<IXEXImage>(std::make_unique<XEXImage>(std::move(*image)));
  }

  Result<std::unique_ptr<IContainerImage>> OpenContainer(
      const std::filesystem::path& path) override {
    auto container = Container::Open(path);
    if (!container) {
      return Err<std::unique_ptr<IContainerImage>>(container.error());
    }
    return std::unique_ptr<IContainerImage>(std::make_unique<Container>(std::move(*container)));
  }

  std::unique_ptr<IXDBFTitleData> OpenXDBF(const u8* data, size_t data_size) override {
    return std::make_unique<XDBFTitleData>(data, data_size);
  }

  Result<PEInfo> ParsePE(std::span<const u8> image) override { return filesystem::ParsePE(image); }

  std::optional<XEXOptHeaderEntry> FindXEXOptHeader(std::span<const u8> header,
                                                    kernel::XEXHeaderKey key) override {
    return FindOptHeaderEntry(header, key);
  }

  Result<XContentPackageHeader> ReadPackageHeader(std::span<const u8> bytes) override {
    return ReadSTFSHeader(bytes);
  }

  Result<XContentPackageHeader> ReadPackageHeader(const std::filesystem::path& path) override {
    return ReadSTFSHeader(path);
  }

 private:
  ILogger* logger_ = nullptr;
};

}  // namespace rex::filesystem
