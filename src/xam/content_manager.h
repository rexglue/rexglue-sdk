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

#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/filesystem/xdbf.h>
#include <rex/kernel/types/content.h>
#include <rex/platform/file.h>
#include <rex/platform/lock_order.h>
#include <rex/result.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/title_lifecycle.h>
#include "xam/xam_title.h"
#include <rex/kernel/types/status.h>
#include <rex/string/text.h>
#include <rex/xam/user_profile.h>

namespace rex {
namespace kernel {
class ILoadedModule;
}  // namespace kernel
}  // namespace rex

namespace rex::xam {

namespace detail {

inline constexpr std::string_view kThumbnailFileName = "__thumbnail.png";

inline constexpr std::string_view kGameContentHeaderDirName = "Headers";

template <typename... Names>
std::filesystem::path ResolveChain(std::filesystem::path path, const Names&... names) {
  ((path = rex::platform::ResolveEntryCase(path, names)), ...);
  return path;
}

}  // namespace detail

class ContentPackage {
 public:
  ContentPackage(filesystem::IFileSystem& file_system, u32 device_index,
                 const std::string_view root_name, const kernel::XCONTENT_AGGREGATE_DATA& data,
                 const std::filesystem::path& package_path);
  ~ContentPackage();

  void LoadPackageLicenseMask(const std::filesystem::path& header_path);

  const kernel::XCONTENT_AGGREGATE_DATA& GetPackageContentData() const { return content_data_; }

  const std::filesystem::path& GetPackagePath() const { return package_path_; }

  u32 GetPackageLicense() const { return license_; }

 private:
  filesystem::IFileSystem& file_system_;
  std::string root_name_;
  std::string device_path_;
  std::filesystem::path package_path_;
  kernel::XCONTENT_AGGREGATE_DATA content_data_;
  u32 license_ = 0;
};

class ContentManager final : public rex::xam::IContentManager {
 public:
  /**
   * @param title The kernel interfaces of the title. The user data root is taken from it.
   * @param user_profile The signed in profile.
   */
  ContentManager(const XAMTitle& title, rex::xam::UserProfile& user_profile);
  ~ContentManager() override;

  std::vector<kernel::XCONTENT_AGGREGATE_DATA> ListContent(u32 device_id, u64 xuid,
                                                           rex::kernel::XContentType content_type,
                                                           u32 title_id = 0xFFFFFFFF) override;
  bool HasContent(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> CreateContent(std::string_view root_name, u64 xuid,
                                           const kernel::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<u32> OpenContent(std::string_view root_name, u64 xuid,
                                        const kernel::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> CloseContent(std::string_view root_name) override;
  [[nodiscard]] Result<std::vector<u8>> GetContentThumbnail(
      u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> SetContentThumbnail(u64 xuid,
                                                 const kernel::XCONTENT_AGGREGATE_DATA& data,
                                                 std::vector<u8> buffer) override;
  [[nodiscard]] Result<void> DeleteContent(u64 xuid,
                                           const kernel::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> WriteContentHeaderFile(u64 xuid, kernel::XCONTENT_AGGREGATE_DATA data,
                                                    u32 license_mask = 0) override;
  [[nodiscard]] Result<kernel::XCONTENT_AGGREGATE_DATA> ReadContentHeaderFile(
      std::string_view file_name, u64 xuid, u32 title_id,
      rex::kernel::XContentType content_type) const override;
  std::filesystem::path ResolveGameUserContentPath() override;
  bool IsContentOpen(const kernel::XCONTENT_AGGREGATE_DATA& data) const override;
  void CloseOpenedFilesFromContent(std::string_view root_name) override;
  [[nodiscard]] Result<PackageInfo> InspectPackage(
      const std::filesystem::path& package_path) const override;
  [[nodiscard]] Result<kernel::XCONTENT_AGGREGATE_DATA> InstallContent(
      const std::filesystem::path& package_path, const InstallOptions& options = {}) override;
  [[nodiscard]] Result<void> SetContentEnabled(const kernel::XCONTENT_AGGREGATE_DATA& data,
                                               bool enabled, std::optional<u64> xuid) override;
  [[nodiscard]] std::vector<InstalledPackage> ListInstalledPackages(
      u32 title_id = kernel::kRunningTitleID) const override;
  [[nodiscard]] rex::ScopeGuard OnContentChanged(rex::MoveOnlyFunction<void()> callback) override;
  /** Returns the title ID of the executable, 0 when the executable has no execution info. */
  u32 GetTitleID() const;
  Result<u32> GetContentDeviceID(std::string_view root_name) const override;

  /**
   * Returns the XDBF data of the executable, which reports invalid when the executable has no XDBF
   * section.
   */
  std::unique_ptr<rex::filesystem::IXDBFTitleData> GetTitleXDBF() const;

 private:
  /** One enable flag, for all profiles when xuid is empty. */
  struct EnableEntry {
    u32 title_id = 0;
    kernel::XContentType content_type = kernel::XContentType::kInvalid;
    std::string file_name;
    std::optional<u64> xuid;
    bool enabled = true;
  };

  std::unique_ptr<ContentPackage> ResolvePackage(const std::string_view root_name, u64 xuid,
                                                 const kernel::XCONTENT_AGGREGATE_DATA& data);
  std::unique_ptr<rex::filesystem::IXDBFTitleData> GetModuleXDBF(
      const kernel::ILoadedModule& exec_module) const;
  std::filesystem::path ResolvePackageRoot(u64 xuid, rex::kernel::XContentType content_type,
                                           u32 title_id = 0xFFFFFFFF) const;
  std::filesystem::path ResolvePackagePath(u64 xuid,
                                           const kernel::XCONTENT_AGGREGATE_DATA& data) const;
  std::filesystem::path ResolveHeaderPathFor(u64 xuid,
                                             const kernel::XCONTENT_AGGREGATE_DATA& data) const;
  std::filesystem::path ResolvePackageHeaderPath(const std::string_view file_name, u64 xuid,
                                                 u32 title_id,
                                                 rex::kernel::XContentType content_type) const;

  std::filesystem::path ResolveManifestPath(u64 xuid,
                                            const kernel::XCONTENT_AGGREGATE_DATA& data) const;
  std::optional<std::filesystem::path> FindInPlaceSource(
      u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const;
  std::optional<u32> FindRunningTitleID() const;
  bool IsContentEnabled(u32 title_id, kernel::XContentType content_type,
                        std::string_view file_name) const;
  bool IsContentEnabledLocked(u32 title_id, kernel::XContentType content_type,
                              std::string_view file_name) const;
  std::filesystem::path GetEnableStatePath() const;
  std::vector<EnableEntry>& LoadEnableEntriesLocked() const;
  void SaveEnableEntriesLocked() const;
  void NotifyContentChanged();

  std::unique_ptr<ContentPackage> DetachPackage(const std::string_view root_name);

  XAMTitle title_;
  std::atomic<int> device_count_{0};
  std::filesystem::path root_path_;
  rex::xam::UserProfile& user_profile_;

  mutable rex::platform::RankedMutex mutex_{rex::platform::LockRank::kLeaf};
  std::unordered_map<string::StringKeyIgnoreCase, std::unique_ptr<ContentPackage>,
                     string::StringKeyIgnoreCase::Hash>
      open_packages_;
  mutable std::optional<std::vector<EnableEntry>> enable_entries_;
  std::vector<std::pair<u64, std::shared_ptr<rex::MoveOnlyFunction<void()>>>> listeners_;
  u64 listener_serial_ = 0;
};

}  // namespace rex::xam
