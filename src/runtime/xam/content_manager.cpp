/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

#include <format>

#include <rex/filesystem.h>
#include <rex/filesystem/container.h>
#include <rex/logging.h>
#include <rex/no_throw.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/runtime/file_object.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/object.h>

#include "runtime/xam/content_manager.h"

namespace rex::runtime {

namespace {

int g_content_device_id = 0;

}  // namespace

ContentPackage::ContentPackage(Runtime& runtime, const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : runtime_(runtime), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = std::format("\\Device\\Content\\{0}\\", ++g_content_device_id);
  content_data_ = data;

  auto* fs = runtime_.GetFileSystem();
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(package_path, ec);
  filesystem::MountEntry entry;
  entry.path = device_path_;
  entry.kind = folder ? filesystem::ProviderKind::kHostDirectory : filesystem::ProviderKind::kSTFS;
  entry.source = package_path;
  entry.flags = folder ? filesystem::MountFlags::kWritable : filesystem::MountFlags::kNone;
  if (auto mounted = fs->Mount(entry); !mounted) {
    REXLOG_WARN(system, "content package '{}' is not mounted, {}", rex::PathToUTF8(package_path),
                mounted.error().message);
  }
  if (auto linked = fs->Link(root_name_ + ":", device_path_); !linked) {
    REXLOG_WARN(system, "content root '{}:' is not linked, {}", root_name_, linked.error().message);
  }
}

ContentPackage::~ContentPackage() {
  log::NoThrow(log::system, [this] {
    auto* fs = runtime_.GetFileSystem();
    if (auto unlinked = fs->Unlink(root_name_ + ":"); !unlinked) {
      REXLOG_DEBUG(system, "{}", unlinked.error().message);
    }
    if (auto unmounted = fs->Unmount(device_path_); !unmounted) {
      REXLOG_DEBUG(system, "{}", unmounted.error().message);
    }
  });
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path& header_path) {
  if (!std::filesystem::exists(header_path)) {
    return;
  }

  std::ifstream file(header_path, std::ios::binary);
  if (!file) {
    return;
  }

  std::error_code ec;
  auto file_size = std::filesystem::file_size(header_path, ec);
  if (ec || file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    return;
  }

  file.seekg(sizeof(XCONTENT_AGGREGATE_DATA));
  file.read(reinterpret_cast<char*>(&license_), sizeof(license_));
}

namespace {

constexpr std::string_view kThumbnailFileName = "__thumbnail.png";

constexpr std::string_view kGameUserContentDirName = "profile";

constexpr std::string_view kGameContentHeaderDirName = "Headers";

}  // namespace

ContentManager::ContentManager(Runtime& runtime, const std::filesystem::path& root_path,
                               runtime::AppManager& app_manager, runtime::UserProfile& user_profile)
    : runtime_(runtime),
      root_path_(root_path),
      app_manager_(app_manager),
      user_profile_(user_profile) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(u64 xuid, XContentType content_type,
                                                         u32 title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = this->GetTitleId();
  }
  auto xuid_str = std::format("{:016X}", xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));

  // Layout is content_root/xuid/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(u64 xuid,
                                                         const XCONTENT_AGGREGATE_DATA& data) {
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;

  if (data.content_type == XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return package_root / rex::to_path(data.GetFileName());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(const std::string_view file_name,
                                                               u64 xuid, u32 title_id,
                                                               XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = this->GetTitleId();
  }

  if (content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = std::format("{:016X}", xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Layout is content_root/xuid/title_id/Headers/content_type/filename.header
  return root_path_ / xuid_str / title_id_str / kGameContentHeaderDirName / content_type_str /
         final_name;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(u32 device_id, u64 xuid,
                                                                 XContentType content_type,
                                                                 u32 title_id) {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = this->GetTitleId();
  }

  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(package_root, ec)) {
    if (!child.is_directory(ec)) {
      continue;
    }
    const std::filesystem::path name = child.path().filename();

    XCONTENT_AGGREGATE_DATA content_data{};
    if (auto header = ReadContentHeaderFile(rex::PathToUTF8(name), xuid, title_id, content_type)) {
      result.emplace_back(*header);
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.SetDisplayName(rex::PathToUTF16(name));
      content_data.SetFileName(rex::PathToUTF8(name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(content_data);
    }
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, u64 xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return nullptr;
  }
  auto package = std::make_unique<ContentPackage>(runtime_, root_name, data, package_path);
  return package;
}

bool ContentManager::ContentExists(u64 xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto path = ResolvePackagePath(xuid, data);
  return std::filesystem::exists(path);
}

Result<void> ContentManager::WriteContentHeaderFile(u64 xuid, XCONTENT_AGGREGATE_DATA data,
                                                    u32 license_mask) {
  if (data.title_id == static_cast<u32>(-1)) {
    data.title_id = this->GetTitleId();
  }
  if (data.xuid == static_cast<u64>(-1)) {
    data.xuid = xuid;
  }
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;

  auto header_path =
      ResolvePackageHeaderPath(data.GetFileName(), used_xuid, data.title_id, data.content_type);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return Err(ResultError(X_ERROR_ACCESS_DENIED));
    }
  }

  std::ofstream file(header_path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return Err(ResultError(X_ERROR_FILE_NOT_FOUND));
  }
  file.write(reinterpret_cast<const char*>(&data), sizeof(XCONTENT_AGGREGATE_DATA));
  if (license_mask != 0) {
    file.write(reinterpret_cast<const char*>(&license_mask), sizeof(license_mask));
  }
  return Ok();
}

Result<XCONTENT_AGGREGATE_DATA> ContentManager::ReadContentHeaderFile(
    const std::string_view file_name, u64 xuid, u32 title_id, XContentType content_type) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr u32 kHeaderSize = sizeof(XCONTENT_AGGREGATE_DATA);

  if (!std::filesystem::exists(header_file_path)) {
    return Err<XCONTENT_AGGREGATE_DATA>(ResultError(X_ERROR_FILE_NOT_FOUND));
  }

  std::ifstream file(header_file_path, std::ios::binary);
  if (!file) {
    return Err<XCONTENT_AGGREGATE_DATA>(ResultError(X_ERROR_FILE_NOT_FOUND));
  }

  std::array<u8, kHeaderSize> buffer;
  if (!file.read(reinterpret_cast<char*>(buffer.data()), kHeaderSize)) {
    return Err<XCONTENT_AGGREGATE_DATA>(ResultError(X_ERROR_FILE_NOT_FOUND));
  }

  XCONTENT_AGGREGATE_DATA data;
  std::memcpy(&data, buffer.data(), buffer.size());
  return data;
}

Result<void> ContentManager::CreateContent(const std::string_view root_name, u64 xuid,
                                           const XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(StringKeyCase(root_name))) {
      return Err(ResultError(X_ERROR_ALREADY_EXISTS));
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    return Err(ResultError(X_ERROR_ALREADY_EXISTS));
  }
  if (!std::filesystem::create_directories(package_path)) {
    return Err(ResultError(X_ERROR_ACCESS_DENIED));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  REX_ASSERT(package != nullptr);

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(StringKeyCase(root_name))) {
      return Err(ResultError(X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(StringKeyCase::Create(root_name), std::move(package));
  }
  return Ok();
}

Result<u32> ContentManager::OpenContent(const std::string_view root_name, u64 xuid,
                                        const XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(StringKeyCase(root_name))) {
      return Err<u32>(ResultError(X_ERROR_ALREADY_EXISTS));
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return Err<u32>(ResultError(X_ERROR_FILE_NOT_FOUND));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  REX_ASSERT(package != nullptr);
  package->LoadPackageLicenseMask(
      ResolvePackageHeaderPath(data.GetFileName(), xuid, this->GetTitleId(), data.content_type));
  const u32 license = package->GetPackageLicense();

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(StringKeyCase(root_name))) {
      return Err<u32>(ResultError(X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(StringKeyCase::Create(root_name), std::move(package));
  }
  return license;
}

Result<void> ContentManager::CloseContent(const std::string_view root_name) {
  std::string open_root_name;
  {
    std::lock_guard lock(mutex_);
    auto it = open_packages_.find(StringKeyCase(root_name));
    if (it == open_packages_.end()) {
      return Err(ResultError(X_ERROR_FILE_NOT_FOUND));
    }
    open_root_name = it->first.GetView();
  }
  CloseOpenedFilesFromContent(open_root_name);
  DetachPackage(open_root_name);
  return Ok();
}

Result<std::vector<u8>> ContentManager::GetContentThumbnail(u64 xuid,
                                                            const XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / kThumbnailFileName;
  std::lock_guard lock(mutex_);
  if (std::filesystem::exists(thumb_path)) {
    std::ifstream file(thumb_path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  } else {
    return Err<std::vector<u8>>(ResultError(X_ERROR_FILE_NOT_FOUND));
  }
}

Result<void> ContentManager::SetContentThumbnail(u64 xuid, const XCONTENT_AGGREGATE_DATA& data,
                                                 std::vector<u8> buffer) {
  auto package_path = ResolvePackagePath(xuid, data);
  std::lock_guard lock(mutex_);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / kThumbnailFileName;
    std::ofstream file(thumb_path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    return Ok();
  } else {
    return Err(ResultError(X_ERROR_FILE_NOT_FOUND));
  }
}

Result<void> ContentManager::DeleteContent(u64 xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.GetFileName(), used_xuid, data.title_id, data.content_type);

  std::lock_guard lock(mutex_);

  if (IsContentOpen(data)) {
    return Err(ResultError(X_ERROR_ACCESS_DENIED));
  }

  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec ||
      std::filesystem::status(package_path, ec).type() != std::filesystem::file_type::not_found) {
    return Err(ResultError(X_ERROR_ACCESS_DENIED));
  }

  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return Ok();
  }
  return Err(ResultError(X_ERROR_FILE_NOT_FOUND));
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = std::format("{:08X}", this->GetTitleId());
  auto user_name = rex::to_path(user_profile_.GetName());

  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

std::unique_ptr<ContentPackage> ContentManager::DetachPackage(const std::string_view root_name) {
  std::lock_guard lock(mutex_);
  auto it = open_packages_.find(StringKeyCase(root_name));
  if (it == open_packages_.end()) {
    return nullptr;
  }
  std::unique_ptr<ContentPackage> package = std::move(it->second);
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  const std::optional<std::string> resolved_path =
      runtime_.GetFileSystem()->FindLink(std::string(root_name) + ':');
  if (!resolved_path || resolved_path->empty()) {
    return;
  }

  const auto is_separator = [](char c) { return c == '\\' || c == '/'; };
  const std::string_view root = *resolved_path;
  const std::vector<Pinned<FileObject>> all_files_handles =
      runtime_.GetObjectTable().All<FileObject>();
  for (const Pinned<FileObject>& file : all_files_handles) {
    const std::string file_path = file->file->GetInfo().absolute_path;
    if (!rex::utf8_starts_with(file_path, root)) {
      continue;
    }
    const std::string_view rest = std::string_view(file_path).substr(root.size());
    if (rest.empty() || is_separator(rest.front()) || is_separator(root.back())) {
      CloseOwned(runtime_.GetObjectTable(), file.GetHandle());
    }
  }
}

Result<XCONTENT_AGGREGATE_DATA> ContentManager::InstallContent(
    const std::filesystem::path& package_path) {
  REX_TRY(filesystem::Container container, filesystem::Container::Open(package_path));
  const filesystem::STFSHeader* header = container.GetHeader();
  if (!header) {
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
        filesystem::FileSystemError::kCorrupt,
        std::format("'{}' is a disc image, not a content package", rex::PathToUTF8(package_path))));
  }
  const auto& metadata = header->metadata;

  XCONTENT_AGGREGATE_DATA content_data;
  content_data.device_id = std::to_underlying(runtime::DummyDeviceId::kHDD);
  content_data.content_type = metadata.content_type;
  content_data.title_id = metadata.execution_info.title_id;
  content_data.xuid = 0;
  content_data.SetFileName(rex::PathToUTF8(package_path.filename()));
  const std::u16string display_name = metadata.GetDisplayName(XLanguage::kEnglish);
  content_data.SetDisplayName(display_name.empty() ? rex::PathToUTF16(package_path.filename())
                                                   : display_name);

  const std::filesystem::path install_path = ResolvePackagePath(0, content_data);

  std::lock_guard lock(mutex_);
  if (IsContentOpen(content_data)) {
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
        filesystem::FileSystemError::kExists,
        std::format("'{}' is open", rex::PathToUTF8(package_path.filename()))));
  }

  std::filesystem::path staging = install_path;
  staging += ".installing";
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  REX_TRY_VOID(container.Extract(staging));
  std::filesystem::remove_all(install_path, ec);
  std::filesystem::rename(staging, install_path, ec);
  if (ec) {
    std::filesystem::remove_all(staging, ec);
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
        filesystem::FileSystemError::kIO,
        std::format("cannot move the package into '{}'", rex::PathToUTF8(install_path))));
  }

  const u32 thumbnail_size =
      std::min<u32>(metadata.thumbnail_size, static_cast<u32>(sizeof(metadata.thumbnail)));
  if (thumbnail_size != 0) {
    std::ofstream thumbnail(install_path / kThumbnailFileName, std::ios::binary | std::ios::trunc);
    thumbnail.write(reinterpret_cast<const char*>(metadata.thumbnail), thumbnail_size);
  }

  u32 license_mask = 0;
  for (const auto& license : header->header.licenses) {
    if (license.license_flags) {
      license_mask |= license.license_bits;
    }
  }
  if (!WriteContentHeaderFile(0, content_data, license_mask)) {
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
        filesystem::FileSystemError::kIO,
        std::format("cannot write the header file of '{}'", rex::PathToUTF8(install_path))));
  }
  return content_data;
}

u32 ContentManager::GetTitleId() const {
  auto module = runtime_.GetObjectTable().Get<ModuleObject>(
      runtime_.GetModuleRegistry().GetExecutable().value_or(0));
  REX_ASSERT(module);

  filesystem::XEXOptExecutionInfo* exec_info = 0;
  module->OptHeader(filesystem::kXEXHeaderExecutionInfo, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

runtime::XDBFGameData ContentManager::GetTitleXDBF() const {
  auto module = runtime_.GetObjectTable().Get<ModuleObject>(
      runtime_.GetModuleRegistry().GetExecutable().value_or(0));
  REX_ASSERT(module);
  return GetModuleXDBF(*module);
}

runtime::XDBFGameData ContentManager::GetModuleXDBF(const ModuleObject& exec_module) const {
  u32 resource_data = 0;
  u32 resource_size = 0;
  if (XSUCCEEDED(exec_module.GetSection(std::format("{:08X}", exec_module.GetTitleId()),
                                        &resource_data, &resource_size))) {
    runtime::XDBFGameData db(runtime_.GetMemory()->TranslateVirtual(resource_data), resource_size);
    return db;
  }
  return runtime::XDBFGameData(nullptr, resource_size);
}

}  // namespace rex::runtime
