/**
 * @file        runtime/content_manager.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/runtime/xdbf_reader.h>
#include <rex/runtime/xam/app_manager.h>
#include <rex/runtime/xam/user_profile.h>
#include <rex/runtime/xcontent_data.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex::runtime {

/**
 * Creates, opens, lists and deletes content packages under the user data root, and reports the
 * running title, its XDBF data, the signed in profile and the XAM apps.
 */
REX_INTERFACE(IContentManager, "rex.runtime.content_manager", 1)
 public:
  /**
   * Lists the content packages of a type.
   *
   * @param device_id The device the packages reside on.
   * @param xuid The owner, 0 for shared content.
   * @param content_type The content type.
   * @param title_id The title, 0xFFFFFFFF for the running title.
   * @return The content data of every package found.
   */
  [[nodiscard]] virtual std::vector<runtime::XCONTENT_AGGREGATE_DATA> ListContent(
      u32 device_id, u64 xuid, XContentType content_type, u32 title_id = 0xFFFFFFFF) = 0;

  /**
   * Reports whether a package exists.
   *
   * @param xuid The owner.
   * @param data The content data.
   * @return True when it exists.
   */
  [[nodiscard]] virtual bool ContentExists(u64 xuid,
                                           const runtime::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Creates a package and mounts it under a root name.
   *
   * @param root_name The root name.
   * @param xuid The owner.
   * @param data The content data.
   * @return Ok, or X_ERROR_ALREADY_EXISTS or X_ERROR_ACCESS_DENIED.
   */
  [[nodiscard]] virtual Result<void> CreateContent(
      std::string_view root_name, u64 xuid, const runtime::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Opens an existing package and mounts it under a root name.
   *
   * @param root_name The root name.
   * @param xuid The owner.
   * @param data The content data.
   * @return The license mask, or X_ERROR_ALREADY_EXISTS or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<u32> OpenContent(std::string_view root_name, u64 xuid,
                                                const runtime::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Unmounts the package under a root name and closes its open files.
   *
   * @param root_name The root name.
   * @return Ok, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> CloseContent(std::string_view root_name) = 0;

  /**
   * Reads the thumbnail of a package.
   *
   * @param xuid The owner.
   * @param data The content data.
   * @return The thumbnail bytes, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<std::vector<u8>> GetContentThumbnail(
      u64 xuid, const runtime::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Writes the thumbnail of a package.
   *
   * @param xuid The owner.
   * @param data The content data.
   * @param buffer The thumbnail bytes.
   * @return Ok, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> SetContentThumbnail(
      u64 xuid, const runtime::XCONTENT_AGGREGATE_DATA& data, std::vector<u8> buffer) = 0;

  /**
   * Deletes a package that is not open.
   *
   * @param xuid The owner.
   * @param data The content data.
   * @return Ok, or X_ERROR_ACCESS_DENIED or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> DeleteContent(
      u64 xuid, const runtime::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Writes the header file of a package.
   *
   * @param xuid The owner.
   * @param data The content data, title 0xFFFFFFFF for the running title.
   * @param license_mask The license mask.
   * @return Ok, or X_ERROR_ACCESS_DENIED or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> WriteContentHeaderFile(u64 xuid,
                                                            runtime::XCONTENT_AGGREGATE_DATA data,
                                                            u32 license_mask = 0) = 0;

  /**
   * Reads the header file of a package.
   *
   * @param file_name The package file name.
   * @param xuid The owner.
   * @param title_id The title.
   * @param content_type The content type.
   * @return The content data, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<runtime::XCONTENT_AGGREGATE_DATA> ReadContentHeaderFile(
      std::string_view file_name, u64 xuid, u32 title_id, XContentType content_type) const = 0;

  /**
   * Returns the folder of the running title's user content for the signed in profile.
   *
   * @return The host path.
   */
  [[nodiscard]] virtual std::filesystem::path ResolveGameUserContentPath() = 0;

  /**
   * Reports whether a package is open.
   *
   * @param data The content data.
   * @return True when it is open.
   */
  [[nodiscard]] virtual bool IsContentOpen(const runtime::XCONTENT_AGGREGATE_DATA& data) const = 0;

  /**
   * Closes every open file under a root name.
   *
   * @param root_name The root name.
   */
  virtual void CloseOpenedFilesFromContent(std::string_view root_name) = 0;

  /**
   * Extracts an STFS or SVOD package into the content tree and writes its header file. The
   * content type and title ID come from the package metadata, so no title needs to run.
   *
   * @param package_path The host package file.
   * @return The content data written to the header file, kExists when the package is open, or
   *         the Container::Open or Extract error.
   */
  [[nodiscard]] virtual Result<runtime::XCONTENT_AGGREGATE_DATA> InstallContent(
      const std::filesystem::path& package_path) = 0;

  /**
   * Returns the title id of the executable.
   *
   * @return The title id, 0 when the executable has no execution info.
   */
  [[nodiscard]] virtual u32 GetTitleID() const = 0;

  /**
   * Returns the XDBF data of the executable.
   *
   * @return The data, invalid when the executable has no XDBF section.
   */
  [[nodiscard]] virtual runtime::XDBFTitleData GetTitleXDBF() const = 0;

  /**
   * Returns the signed in profile.
   *
   * @return The profile.
   */
  [[nodiscard]] virtual runtime::UserProfile& GetUserProfile() = 0;

  /**
   * Returns the XAM app manager.
   *
   * @return The app manager.
   */
  [[nodiscard]] virtual runtime::AppManager& GetAppManager() = 0;

  /**
   * Returns the device a package is stored on.
   *
   * @param root_name The root name of an open package.
   * @return The device id, or X_ERROR_FILE_NOT_FOUND when no package is open under the name.
   */
  [[nodiscard]] virtual Result<u32> GetContentDeviceID(std::string_view root_name) const = 0;
};

}  // namespace rex::runtime
