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
#include <string>
#include <system_error>

#include <format>

#include <rex/filesystem.h>
#include <rex/filesystem/container.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/runtime/xfile.h>
#include <rex/runtime/xmodule.h>
#include <rex/runtime/xobject.h>

#include "runtime/xam/content_manager.h"

namespace rex::runtime::xam {

namespace {

int g_content_device_id = 0;

}  // namespace

ContentPackage::ContentPackage(Runtime& runtime, const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : runtime_(runtime), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = std::format("\\Device\\Content\\{0}\\", ++g_content_device_id);
  content_data_ = data;

  auto* fs = runtime_.file_system();
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(package_path, ec);
  filesystem::MountEntry entry;
  entry.path = device_path_;
  entry.kind = folder ? filesystem::ProviderKind::kHostDirectory : filesystem::ProviderKind::kSTFS;
  entry.source = package_path;
  entry.flags = folder ? filesystem::MountFlags::kWritable : filesystem::MountFlags::kNone;
  if (auto mounted = fs->Mount(entry); !mounted) {
    REXLOG_WARN(system, "content package '{}' is not mounted, {}", rex::path_to_utf8(package_path),
                mounted.error().message);
  }
  if (auto linked = fs->Link(root_name_ + ":", device_path_); !linked) {
    REXLOG_WARN(system, "content root '{}:' is not linked, {}", root_name_, linked.error().message);
  }
}

ContentPackage::~ContentPackage() {
  auto* fs = runtime_.file_system();
  if (auto unlinked = fs->Unlink(root_name_ + ":"); !unlinked) {
    REXLOG_DEBUG(system, "{}", unlinked.error().message);
  }
  if (auto unmounted = fs->Unmount(device_path_); !unmounted) {
    REXLOG_DEBUG(system, "{}", unmounted.error().message);
  }
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

}  // namespace rex::runtime::xam

namespace rex::runtime {

using xam::ContentPackage;
using xam::kCurrentlyRunningTitleId;
using xam::XCONTENT_AGGREGATE_DATA;

namespace {

constexpr std::string_view kThumbnailFileName = "__thumbnail.png";

constexpr std::string_view kGameUserContentDirName = "profile";

constexpr std::string_view kGameContentHeaderDirName = "Headers";

}  // namespace

ContentManager::ContentManager(Runtime& runtime, const std::filesystem::path& root_path,
                               xam::AppManager& app_manager, xam::UserProfile& user_profile)
    : runtime_(runtime),
      root_path_(root_path),
      app_manager_(app_manager),
      user_profile_(user_profile) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(u64 xuid, XContentType content_type,
                                                         u32 title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = this->title_id();
  }
  auto xuid_str = std::format("{:016X}", xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));

  // Package root path:
  // content_root/xuid/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(u64 xuid,
                                                         const XCONTENT_AGGREGATE_DATA& data) {
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;

  // DLCs are stored in common directory
  if (data.content_type == XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  // Content path:
  // content_root/xuid/title_id/content_type/data_file_name/
  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return package_root / rex::to_path(data.file_name());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(const std::string_view file_name,
                                                               u64 xuid, u32 title_id,
                                                               XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = this->title_id();
  }

  if (content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = std::format("{:016X}", xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Header root path:
  // content_root/xuid/title_id/Headers/content_type/filename.header
  return root_path_ / xuid_str / title_id_str / kGameContentHeaderDirName / content_type_str /
         final_name;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(u32 device_id, u64 xuid,
                                                                 XContentType content_type,
                                                                 u32 title_id) {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = this->title_id();
  }

  // Search path:
  // content_root/xuid/title_id/type_name/*
  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(package_root, ec)) {
    if (!child.is_directory(ec)) {
      continue;
    }
    const std::filesystem::path name = child.path().filename();

    XCONTENT_AGGREGATE_DATA content_data{};
    if (auto header =
            ReadContentHeaderFile(rex::path_to_utf8(name), xuid, title_id, content_type)) {
      result.emplace_back(*header);
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(rex::path_to_utf16(name));
      content_data.set_file_name(rex::path_to_utf8(name));
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
    data.title_id = this->title_id();
  }
  if (data.xuid == static_cast<u64>(-1)) {
    data.xuid = xuid;
  }
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;

  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
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
    if (open_packages_.count(string_key_case(root_name))) {
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
    if (open_packages_.count(string_key_case(root_name))) {
      return Err(ResultError(X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string_key_case::create(root_name), std::move(package));
  }
  return Ok();
}

Result<u32> ContentManager::OpenContent(const std::string_view root_name, u64 xuid,
                                        const XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string_key_case(root_name))) {
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
      ResolvePackageHeaderPath(data.file_name(), xuid, this->title_id(), data.content_type));
  const u32 license = package->GetPackageLicense();

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string_key_case(root_name))) {
      return Err<u32>(ResultError(X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string_key_case::create(root_name), std::move(package));
  }
  return license;
}

Result<void> ContentManager::CloseContent(const std::string_view root_name) {
  std::string open_root_name;
  {
    std::lock_guard lock(mutex_);
    // Some games use different casing between Create and Close (e.g. "save" vs "SAVE")
    auto it = open_packages_.find(string_key_case(root_name));
    if (it == open_packages_.end()) {
      return Err(ResultError(X_ERROR_FILE_NOT_FOUND));
    }
    open_root_name = it->first.view();
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
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);

  std::lock_guard lock(mutex_);

  if (IsContentOpen(data)) {
    // TODO(Gliniak): Get real error code for this case.
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
  auto title_id = std::format("{:08X}", this->title_id());
  auto user_name = rex::to_path(user_profile_.name());

  // Per-game per-profile data location:
  // content_root/title_id/profile/user_name
  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

std::unique_ptr<ContentPackage> ContentManager::DetachPackage(const std::string_view root_name) {
  std::lock_guard lock(mutex_);
  auto it = open_packages_.find(string_key_case(root_name));
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
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<Pinned<FileObject>> all_files_handles =
      runtime_.object_table().GetAll<FileObject>();

  const std::string resolved_path =
      runtime_.file_system()->FindLink(std::string(root_name) + ':').value_or("");

  for (const Pinned<FileObject>& file : all_files_handles) {
    const std::string file_path = file->file->Info().absolute_path;
    bool is_file_inside_content = rex::utf8_starts_with(file_path, resolved_path);

    if (is_file_inside_content) {
      (void)runtime_.object_table().Close(file.handle());
    }
  }
}

Result<XCONTENT_AGGREGATE_DATA> ContentManager::InstallContent(
    const std::filesystem::path& package_path) {
  REX_TRY(filesystem::Container container, filesystem::Container::Open(package_path));
  const filesystem::STFSHeader* header = container.header();
  if (!header) {
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::MakeError(
        filesystem::Error::kCorrupt, std::format("'{}' is a disc image, not a content package",
                                                 rex::path_to_utf8(package_path))));
  }
  const auto& metadata = header->metadata;

  XCONTENT_AGGREGATE_DATA content_data;
  content_data.device_id = std::to_underlying(xam::DummyDeviceId::HDD);
  content_data.content_type = metadata.content_type;
  content_data.title_id = metadata.execution_info.title_id;
  content_data.xuid = 0;
  content_data.set_file_name(rex::path_to_utf8(package_path.filename()));
  const std::u16string display_name = metadata.display_name(XLanguage::kEnglish);
  content_data.set_display_name(display_name.empty() ? rex::path_to_utf16(package_path.filename())
                                                     : display_name);

  const std::filesystem::path install_path = ResolvePackagePath(0, content_data);

  std::lock_guard lock(mutex_);
  if (IsContentOpen(content_data)) {
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::MakeError(
        filesystem::Error::kExists,
        std::format("'{}' is open", rex::path_to_utf8(package_path.filename()))));
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
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::MakeError(
        filesystem::Error::kIO,
        std::format("cannot move the package into '{}'", rex::path_to_utf8(install_path))));
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
    return Err<XCONTENT_AGGREGATE_DATA>(filesystem::MakeError(
        filesystem::Error::kIO,
        std::format("cannot write the header file of '{}'", rex::path_to_utf8(install_path))));
  }
  return content_data;
}

u32 ContentManager::title_id() const {
  auto module = runtime_.object_table().Get<ModuleObject>(
      runtime_.module_registry().executable().value_or(0));
  REX_ASSERT(module);

  xex2_opt_execution_info* exec_info = 0;
  UserModuleGetOptHeader(*module, XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

util::XDBFGameData ContentManager::title_xdbf() const {
  auto module = runtime_.object_table().Get<ModuleObject>(
      runtime_.module_registry().executable().value_or(0));
  REX_ASSERT(module);
  return module_xdbf(*module);
}

util::XDBFGameData ContentManager::module_xdbf(const ModuleObject& exec_module) const {
  u32 resource_data = 0;
  u32 resource_size = 0;
  if (XSUCCEEDED(ModuleGetSection(exec_module,
                                  std::format("{:08X}", UserModuleTitleID(exec_module)),
                                  &resource_data, &resource_size))) {
    util::XDBFGameData db(runtime_.memory()->TranslateVirtual(resource_data), resource_size);
    return db;
  }
  return util::XDBFGameData(nullptr, resource_size);
}

}  // namespace rex::runtime
