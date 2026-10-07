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

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/result.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/xcontent.h>
#include <rex/runtime/xtypes.h>
#include <rex/string/key.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {
struct ModuleObject;
}  // namespace rex::runtime

namespace rex::runtime::xam {

class ContentPackage {
 public:
  ContentPackage(Runtime& runtime, const std::string_view root_name,
                 const XCONTENT_AGGREGATE_DATA& data, const std::filesystem::path& package_path);
  ~ContentPackage();

  void LoadPackageLicenseMask(const std::filesystem::path& header_path);

  const XCONTENT_AGGREGATE_DATA& GetPackageContentData() const { return content_data_; }

  const std::filesystem::path& package_path() const { return package_path_; }

  u32 GetPackageLicense() const { return license_; }

 private:
  Runtime& runtime_;
  std::string root_name_;
  std::string device_path_;
  std::filesystem::path package_path_;
  XCONTENT_AGGREGATE_DATA content_data_;
  u32 license_ = 0;
};

}  // namespace rex::runtime::xam

namespace rex::runtime {

/** The content manager of a runtime. */
class ContentManager final : public IContentManager {
 public:
  /**
   * @param runtime The runtime whose file system and table packages use.
   * @param root_path The user data root, empty for none.
   * @param app_manager The Xam app manager.
   * @param user_profile The signed in profile.
   */
  ContentManager(Runtime& runtime, const std::filesystem::path& root_path,
                 xam::AppManager& app_manager, xam::UserProfile& user_profile);
  ~ContentManager() override;

  std::vector<xam::XCONTENT_AGGREGATE_DATA> ListContent(u32 device_id, u64 xuid,
                                                        XContentType content_type,
                                                        u32 title_id = 0xFFFFFFFF) override;
  bool ContentExists(u64 xuid, const xam::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> CreateContent(std::string_view root_name, u64 xuid,
                                           const xam::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<u32> OpenContent(std::string_view root_name, u64 xuid,
                                        const xam::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> CloseContent(std::string_view root_name) override;
  [[nodiscard]] Result<std::vector<u8>> GetContentThumbnail(
      u64 xuid, const xam::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> SetContentThumbnail(u64 xuid, const xam::XCONTENT_AGGREGATE_DATA& data,
                                                 std::vector<u8> buffer) override;
  [[nodiscard]] Result<void> DeleteContent(u64 xuid,
                                           const xam::XCONTENT_AGGREGATE_DATA& data) override;
  [[nodiscard]] Result<void> WriteContentHeaderFile(u64 xuid, xam::XCONTENT_AGGREGATE_DATA data,
                                                    u32 license_mask = 0) override;
  [[nodiscard]] Result<xam::XCONTENT_AGGREGATE_DATA> ReadContentHeaderFile(
      std::string_view file_name, u64 xuid, u32 title_id, XContentType content_type) const override;
  std::filesystem::path ResolveGameUserContentPath() override;
  bool IsContentOpen(const xam::XCONTENT_AGGREGATE_DATA& data) const override;
  void CloseOpenedFilesFromContent(std::string_view root_name) override;
  [[nodiscard]] Result<xam::XCONTENT_AGGREGATE_DATA> InstallContent(
      const std::filesystem::path& package_path) override;
  u32 title_id() const override;
  util::XDBFGameData title_xdbf() const override;
  xam::UserProfile& user_profile() override { return user_profile_; }
  xam::AppManager& app_manager() override { return app_manager_; }

 private:
  std::unique_ptr<xam::ContentPackage> ResolvePackage(const std::string_view root_name, u64 xuid,
                                                      const xam::XCONTENT_AGGREGATE_DATA& data);
  util::XDBFGameData module_xdbf(const ModuleObject& exec_module) const;
  std::filesystem::path ResolvePackageRoot(u64 xuid, XContentType content_type,
                                           u32 title_id = 0xFFFFFFFF);
  std::filesystem::path ResolvePackagePath(u64 xuid, const xam::XCONTENT_AGGREGATE_DATA& data);
  std::filesystem::path ResolvePackageHeaderPath(const std::string_view file_name, u64 xuid,
                                                 u32 title_id, XContentType content_type) const;

  /**
   * Removes an open package from the table.
   *
   * @param root_name The root name the package is mounted under.
   * @return The detached package, or null when none is open under the name.
   */
  std::unique_ptr<xam::ContentPackage> DetachPackage(const std::string_view root_name);

  Runtime& runtime_;
  std::filesystem::path root_path_;
  xam::AppManager& app_manager_;
  xam::UserProfile& user_profile_;

  rex::platform::RankedMutex mutex_{rex::platform::LockRank::Leaf};
  std::unordered_map<string_key_case, std::unique_ptr<xam::ContentPackage>, string_key_case::Hash>
      open_packages_;
};

}  // namespace rex::runtime
