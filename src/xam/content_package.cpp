/**
 * @file        xam/content_package.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <charconv>
#include <format>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/stfs.h>
#include <rex/kernel/module.h>
#include <rex/kernel/types/content.h>
#include <rex/kernel/types/status.h>
#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <rex/string/text.h>
#include <rex/xam/device.h>
#include <rex/xam/user_profile.h>
#include <rex/filesystem/file_system_factory.h>

#include "xam/content_manager.h"

namespace rex::xam {

namespace {

constexpr std::string_view kManifestSuffix = ".package.toml";
constexpr std::string_view kEnableStateFileName = "content_state.toml";
constexpr std::string_view kInstalledXuidDirName = "0000000000000000";

struct PackageManifest {
  PackageInfo info;
  std::optional<std::filesystem::path> source;
};

PackageInfo BuildPackageInfo(const filesystem::XContentPackageHeader& header) {
  const filesystem::XContentMetadata& metadata = header.metadata;
  PackageInfo info;
  info.title_id = metadata.execution_info.title_id;
  info.content_type = static_cast<kernel::XContentType>(metadata.content_type);
  for (u32 value = 1; value <= filesystem::XContentMetadata::kNumLanguagesV2; ++value) {
    const auto language = static_cast<kernel::XLanguage>(value);
    if (!filesystem::detail::FindLanguageSlot(language)) {
      continue;
    }
    std::u16string name = metadata.GetDisplayName(language);
    if (!name.empty()) {
      info.display_names.emplace(language, std::move(name));
    }
  }
  info.description = metadata.GetDescription(kernel::XLanguage::kEnglish);
  info.publisher = metadata.GetPublisher();
  std::copy(std::begin(header.header.content_id), std::end(header.header.content_id),
            info.content_id.begin());
  for (const auto& license : header.header.licenses) {
    if (license.license_flags) {
      info.license_mask |= license.license_bits;
    }
  }
  const u32 thumbnail_size =
      std::min<u32>(metadata.thumbnail_size, static_cast<u32>(sizeof(metadata.thumbnail)));
  info.thumbnail.assign(metadata.thumbnail, metadata.thumbnail + thumbnail_size);
  return info;
}

std::string WriteManifest(const PackageInfo& info,
                          const std::optional<std::filesystem::path>& source) {
  toml::table table;
  table.insert("title_id", static_cast<int64_t>(info.title_id));
  table.insert("content_type", static_cast<int64_t>(info.content_type));
  table.insert("license_mask", static_cast<int64_t>(info.license_mask));
  std::string content_id;
  for (const u8 byte : info.content_id) {
    content_id += std::format("{:02X}", byte);
  }
  table.insert("content_id", std::move(content_id));
  if (source) {
    table.insert("source", rex::string::PathToUTF8(*source));
  }
  table.insert("description", rex::string::ToUTF8(info.description));
  table.insert("publisher", rex::string::ToUTF8(info.publisher));
  toml::table names;
  for (const auto& [language, name] : info.display_names) {
    names.insert(std::to_string(static_cast<u32>(language)), rex::string::ToUTF8(name));
  }
  table.insert("display_names", std::move(names));
  std::ostringstream text;
  text << table;
  return text.str();
}

std::optional<toml::table> ParseHostToml(const std::filesystem::path& path) {
  auto bytes = platform::ReadHostFile(path);
  if (!bytes) {
    return std::nullopt;
  }
  try {
    return toml::parse(
        std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  } catch (const toml::parse_error& error) {
    REXLOG_WARN(system, "'{}' is not read, {}", rex::string::PathToUTF8(path), error.what());
    return std::nullopt;
  }
}

std::optional<PackageManifest> ReadManifest(const std::filesystem::path& path) {
  const std::optional<toml::table> table = ParseHostToml(path);
  if (!table) {
    return std::nullopt;
  }
  PackageManifest manifest;
  manifest.info.title_id = static_cast<u32>((*table)["title_id"].value_or<int64_t>(0));
  manifest.info.content_type =
      static_cast<kernel::XContentType>((*table)["content_type"].value_or<int64_t>(0));
  manifest.info.license_mask = static_cast<u32>((*table)["license_mask"].value_or<int64_t>(0));
  const std::string content_id = (*table)["content_id"].value_or<std::string>("");
  if (content_id.size() == manifest.info.content_id.size() * 2) {
    for (size_t i = 0; i < manifest.info.content_id.size(); ++i) {
      std::from_chars(content_id.data() + i * 2, content_id.data() + i * 2 + 2,
                      manifest.info.content_id[i], 16);
    }
  }
  manifest.info.description =
      rex::string::ToUTF16((*table)["description"].value_or<std::string_view>(""));
  manifest.info.publisher =
      rex::string::ToUTF16((*table)["publisher"].value_or<std::string_view>(""));
  if (const auto* names = (*table)["display_names"].as_table()) {
    for (const auto& [key, node] : *names) {
      u32 value = 0;
      const std::string_view text = key.str();
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || !node.is_string()) {
        continue;
      }
      manifest.info.display_names.emplace(static_cast<kernel::XLanguage>(value),
                                          rex::string::ToUTF16(node.as_string()->get()));
    }
  }
  if (const auto source = (*table)["source"].value<std::string>()) {
    manifest.source = rex::string::to_path(*source);
  }
  return manifest;
}

}  // namespace

Result<PackageInfo> ContentManager::InspectPackage(
    const std::filesystem::path& package_path) const {
  REX_TRY(filesystem::XContentPackageHeader header,
          title_.file_system_factory.ReadPackageHeader(package_path));
  return BuildPackageInfo(header);
}

Result<kernel::XCONTENT_AGGREGATE_DATA> ContentManager::InstallContent(
    const std::filesystem::path& package_path, const InstallOptions& options) {
  REX_TRY(filesystem::XContentPackageHeader header,
          title_.file_system_factory.ReadPackageHeader(package_path));
  const PackageInfo info = BuildPackageInfo(header);
  const std::string package_name = rex::string::PathToUTF8(package_path.filename());

  if (options.require_running_title) {
    const std::optional<u32> running = FindRunningTitleID();
    if (!running) {
      return Err<kernel::XCONTENT_AGGREGATE_DATA>(
          ErrorCategory::kValidation,
          std::format("'{}' cannot be matched to a title, no title is running", package_name));
    }
    if (info.title_id != *running) {
      return Err<kernel::XCONTENT_AGGREGATE_DATA>(
          ErrorCategory::kValidation,
          std::format("'{}' is content for title {:08X}, the running title is {:08X}", package_name,
                      info.title_id, *running));
    }
  }
  if (!options.extract && header.metadata.volume_type != filesystem::XContentVolumeType::kSTFS) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
        filesystem::FileSystemError::kUnsupported,
        std::format("'{}' is not a STFS package, only a STFS package installs in place",
                    package_name)));
  }

  kernel::XCONTENT_AGGREGATE_DATA content_data;
  content_data.device_id = std::to_underlying(rex::xam::DummyDeviceID::kHDD);
  content_data.content_type = info.content_type;
  content_data.title_id = info.title_id;
  content_data.xuid = 0;
  content_data.SetFileName(package_name);
  const auto english = info.display_names.find(kernel::XLanguage::kEnglish);
  content_data.SetDisplayName(english != info.display_names.end() && !english->second.empty()
                                  ? english->second
                                  : rex::string::PathToUTF16(package_path.filename()));

  const std::filesystem::path install_path = ResolvePackagePath(0, content_data);

  {
    std::lock_guard lock(mutex_);
    if (IsContentOpen(content_data)) {
      return Err<kernel::XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
          filesystem::FileSystemError::kExists, std::format("'{}' is open", package_name)));
    }

    std::error_code ec;
    if (options.extract) {
      REX_TRY(std::unique_ptr<filesystem::IContainer> container,
              title_.file_system_factory.OpenContainer(package_path));
      std::filesystem::path staging = install_path;
      staging += ".installing";
      std::filesystem::remove_all(staging, ec);
      REX_TRY_VOID(container->Extract(staging));
      std::filesystem::remove_all(install_path, ec);
      std::filesystem::rename(staging, install_path, ec);
      if (ec) {
        std::filesystem::remove_all(staging, ec);
        return Err<kernel::XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
            filesystem::FileSystemError::kIO, std::format("cannot move the package into '{}'",
                                                          rex::string::PathToUTF8(install_path))));
      }
    } else {
      std::filesystem::remove_all(install_path, ec);
      std::filesystem::create_directories(install_path, ec);
      if (ec) {
        return Err<kernel::XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
            filesystem::FileSystemError::kIO,
            std::format("cannot create '{}'", rex::string::PathToUTF8(install_path))));
      }
    }

    if (!info.thumbnail.empty()) {
      if (auto written =
              platform::WriteHostFile(install_path / detail::kThumbnailFileName, info.thumbnail);
          !written) {
        REXLOG_WARN(system, "the thumbnail write failed, {}", written.error().GetDescription());
      }
    }

    if (!WriteContentHeaderFile(0, content_data, info.license_mask)) {
      return Err<kernel::XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
          filesystem::FileSystemError::kIO, std::format("cannot write the header file of '{}'",
                                                        rex::string::PathToUTF8(install_path))));
    }
    std::optional<std::filesystem::path> source;
    if (!options.extract) {
      source = std::filesystem::absolute(package_path, ec);
    }
    if (auto written = platform::WriteHostFile(ResolveManifestPath(0, content_data),
                                               WriteManifest(info, source));
        !written) {
      return Err<kernel::XCONTENT_AGGREGATE_DATA>(filesystem::BuildError(
          filesystem::FileSystemError::kIO,
          std::format("cannot write the package manifest of '{}'", package_name)));
    }
  }
  NotifyContentChanged();
  return content_data;
}

Result<void> ContentManager::SetContentEnabled(const kernel::XCONTENT_AGGREGATE_DATA& data,
                                               bool enabled, std::optional<u64> xuid) {
  kernel::XCONTENT_AGGREGATE_DATA resolved = data;
  if (resolved.title_id == kernel::kRunningTitleID) {
    const std::optional<u32> running = FindRunningTitleID();
    if (!running) {
      return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
    }
    resolved.title_id = *running;
  }
  {
    std::lock_guard lock(mutex_);
    if (!std::filesystem::exists(ResolvePackagePath(0, resolved))) {
      return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
    }
    LoadEnableEntriesLocked();
    const std::string file_name = resolved.GetFileName();
    auto entry = std::ranges::find_if(*enable_entries_, [&](const EnableEntry& candidate) {
      return candidate.title_id == resolved.title_id &&
             candidate.content_type == resolved.content_type && candidate.file_name == file_name &&
             candidate.xuid == xuid;
    });
    if (entry != enable_entries_->end()) {
      if (entry->enabled == enabled) {
        return Ok();
      }
      entry->enabled = enabled;
    } else {
      enable_entries_->push_back(
          {resolved.title_id, resolved.content_type, file_name, xuid, enabled});
    }
    SaveEnableEntriesLocked();
  }
  NotifyContentChanged();
  return Ok();
}

std::vector<InstalledPackage> ContentManager::ListInstalledPackages(u32 title_id) const {
  if (title_id == kernel::kRunningTitleID) {
    const std::optional<u32> running = FindRunningTitleID();
    if (!running) {
      return {};
    }
    title_id = *running;
  }
  std::vector<InstalledPackage> result;
  const std::filesystem::path headers_root =
      detail::ResolveChain(root_path_, kInstalledXuidDirName, std::format("{:08X}", title_id),
                           detail::kGameContentHeaderDirName);
  std::error_code ec;
  for (const auto& type_dir : std::filesystem::directory_iterator(headers_root, ec)) {
    std::error_code type_ec;
    if (!type_dir.is_directory(type_ec)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(type_dir.path(), type_ec)) {
      const std::string name = rex::string::PathToUTF8(entry.path().filename());
      if (!name.ends_with(kManifestSuffix)) {
        continue;
      }
      const std::string file_name = name.substr(0, name.size() - kManifestSuffix.size());
      std::optional<PackageManifest> manifest = ReadManifest(entry.path());
      if (!manifest) {
        continue;
      }
      auto header = ReadContentHeaderFile(file_name, 0, title_id, manifest->info.content_type);
      if (!header) {
        continue;
      }
      InstalledPackage package;
      package.data = *header;
      package.info = std::move(manifest->info);
      package.in_place = manifest->source.has_value();
      package.enabled = IsContentEnabled(title_id, package.info.content_type, file_name);
      if (auto thumbnail =
              platform::ReadHostFile(ResolvePackagePath(0, *header) / detail::kThumbnailFileName)) {
        package.info.thumbnail = std::move(*thumbnail);
      }
      result.push_back(std::move(package));
    }
  }
  std::ranges::sort(result, [](const InstalledPackage& a, const InstalledPackage& b) {
    return a.data.GetFileName() < b.data.GetFileName();
  });
  return result;
}

rex::ScopeGuard ContentManager::OnContentChanged(rex::MoveOnlyFunction<void()> callback) {
  auto shared = std::make_shared<rex::MoveOnlyFunction<void()>>(std::move(callback));
  u64 serial = 0;
  {
    std::lock_guard lock(mutex_);
    serial = ++listener_serial_;
    listeners_.emplace_back(serial, std::move(shared));
  }
  return rex::ScopeGuard([this, serial] {
    std::lock_guard lock(mutex_);
    std::erase_if(listeners_, [serial](const auto& listener) { return listener.first == serial; });
  });
}

void ContentManager::NotifyContentChanged() {
  std::vector<std::shared_ptr<rex::MoveOnlyFunction<void()>>> callbacks;
  {
    std::lock_guard lock(mutex_);
    for (const auto& listener : listeners_) {
      callbacks.push_back(listener.second);
    }
  }
  for (const auto& callback : callbacks) {
    (*callback)();
  }
}

std::filesystem::path ContentManager::ResolveManifestPath(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  return ResolveHeaderPathFor(xuid, data).parent_path() /
         (data.GetFileName() + std::string(kManifestSuffix));
}

std::optional<std::filesystem::path> ContentManager::FindInPlaceSource(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  const std::optional<PackageManifest> manifest = ReadManifest(ResolveManifestPath(xuid, data));
  if (!manifest) {
    return std::nullopt;
  }
  return manifest->source;
}

std::optional<u32> ContentManager::FindRunningTitleID() const {
  if (!title_.modules.GetExecutable()) {
    return std::nullopt;
  }
  return GetTitleID();
}

bool ContentManager::IsContentEnabled(u32 title_id, kernel::XContentType content_type,
                                      std::string_view file_name) const {
  std::lock_guard lock(mutex_);
  return IsContentEnabledLocked(title_id, content_type, file_name);
}

bool ContentManager::IsContentEnabledLocked(u32 title_id, kernel::XContentType content_type,
                                            std::string_view file_name) const {
  LoadEnableEntriesLocked();
  const u64 profile = user_profile_.GetXUID();
  std::optional<bool> all_profiles;
  for (const EnableEntry& entry : *enable_entries_) {
    if (entry.title_id != title_id || entry.content_type != content_type ||
        entry.file_name != file_name) {
      continue;
    }
    if (!entry.xuid) {
      all_profiles = entry.enabled;
    } else if (*entry.xuid == profile) {
      return entry.enabled;
    }
  }
  return all_profiles.value_or(true);
}

std::filesystem::path ContentManager::GetEnableStatePath() const {
  return root_path_.empty() ? std::filesystem::path{} : root_path_ / kEnableStateFileName;
}

void ContentManager::LoadEnableEntriesLocked() const {
  if (enable_entries_) {
    return;
  }
  enable_entries_.emplace();
  const std::filesystem::path path = GetEnableStatePath();
  if (path.empty()) {
    return;
  }
  const std::optional<toml::table> table = ParseHostToml(path);
  if (!table) {
    return;
  }
  const auto* packages = (*table)["package"].as_array();
  if (!packages) {
    return;
  }
  for (const auto& node : *packages) {
    const auto* entry = node.as_table();
    if (!entry) {
      continue;
    }
    EnableEntry loaded;
    loaded.title_id = static_cast<u32>((*entry)["title_id"].value_or<int64_t>(0));
    loaded.content_type =
        static_cast<kernel::XContentType>((*entry)["content_type"].value_or<int64_t>(0));
    loaded.file_name = (*entry)["file_name"].value_or<std::string>("");
    loaded.enabled = (*entry)["enabled"].value_or(true);
    if (const auto xuid = (*entry)["xuid"].value<std::string>()) {
      u64 value = 0;
      const auto parsed = std::from_chars(xuid->data(), xuid->data() + xuid->size(), value, 16);
      if (parsed.ec != std::errc{}) {
        continue;
      }
      loaded.xuid = value;
    }
    if (!loaded.file_name.empty()) {
      enable_entries_->push_back(std::move(loaded));
    }
  }
}

void ContentManager::SaveEnableEntriesLocked() const {
  const std::filesystem::path path = GetEnableStatePath();
  if (path.empty() || !enable_entries_) {
    return;
  }
  toml::array packages;
  for (const EnableEntry& entry : *enable_entries_) {
    toml::table node;
    node.insert("title_id", static_cast<int64_t>(entry.title_id));
    node.insert("content_type", static_cast<int64_t>(entry.content_type));
    node.insert("file_name", entry.file_name);
    if (entry.xuid) {
      node.insert("xuid", std::format("{:016X}", *entry.xuid));
    }
    node.insert("enabled", entry.enabled);
    packages.push_back(std::move(node));
  }
  toml::table table;
  table.insert("package", std::move(packages));
  std::ostringstream text;
  text << table;
  if (auto written = platform::WriteHostFile(path, text.str()); !written) {
    REXLOG_WARN(system, "the content state is not saved, {}", written.error().GetDescription());
  }
}

}  // namespace rex::xam
