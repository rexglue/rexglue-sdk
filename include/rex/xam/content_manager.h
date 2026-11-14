/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/content_manager.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <format>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <functional>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/stream.h>
#include <rex/kernel/types/content.h>
#include <rex/result.h>
#include <rex/utility.h>
#include <rex/title.h>

namespace rex::xam {

/** What the header of a content package says, read without extracting the package. */
struct PackageInfo {
  u32 title_id = 0;
  kernel::XContentType content_type = kernel::XContentType::kInvalid;
  std::map<kernel::XLanguage, std::u16string> display_names; /**< Only the languages with a name. */
  std::u16string description;                                /**< The English description. */
  std::u16string publisher;
  std::array<u8, 20> content_id{}; /**< The SHA1 of the metadata that names the package. */
  u32 license_mask = 0;            /**< The OR of the license bits that carry flags. */
  std::vector<u8> thumbnail;       /**< A PNG, empty when the package has none. */
};

/** How InstallContent places a package. */
struct InstallOptions {
  /** Refuses a package whose title ID is not the running title. */
  bool require_running_title = true;
  /** Extracts the package. False keeps it at its path and mounts it in place when opened. */
  bool extract = true;
};

/** An installed package as a host menu lists it. */
struct InstalledPackage {
  kernel::XCONTENT_AGGREGATE_DATA data;
  PackageInfo info;
  bool enabled = true;   /**< The effective flag for the signed in profile. */
  bool in_place = false; /**< True when the package stays at its original path. */
};

class IContentManager : public NonCopyable {
 public:
  virtual ~IContentManager() = default;

  /**
   * Lists the content packages of a type.
   *
   * @param xuid The owner, 0 for shared content.
   * @param title_id The title, 0xFFFFFFFF for the running title.
   * @return The content data of every package found.
   */
  [[nodiscard]] virtual std::vector<kernel::XCONTENT_AGGREGATE_DATA> ListContent(
      u32 device_id, u64 xuid, rex::kernel::XContentType content_type,
      u32 title_id = 0xFFFFFFFF) = 0;

  [[nodiscard]] virtual bool HasContent(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Creates a package and mounts it under a root name.
   *
   * @return Ok, or X_ERROR_ALREADY_EXISTS or X_ERROR_ACCESS_DENIED.
   */
  [[nodiscard]] virtual Result<void> CreateContent(std::string_view root_name, u64 xuid,
                                                   const kernel::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Opens an existing package and mounts it under a root name.
   *
   * @return The license mask, or X_ERROR_ALREADY_EXISTS or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<u32> OpenContent(std::string_view root_name, u64 xuid,
                                                const kernel::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Unmounts the package under a root name and closes its open files.
   *
   * @return Ok, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> CloseContent(std::string_view root_name) = 0;

  /**
   * Reads the thumbnail of a package.
   *
   * @return The thumbnail bytes, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<std::vector<u8>> GetContentThumbnail(
      u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Writes the thumbnail of a package.
   *
   * @param buffer The thumbnail bytes.
   * @return Ok, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> SetContentThumbnail(
      u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data, std::vector<u8> buffer) = 0;

  /**
   * Deletes a package that is not open.
   *
   * @return Ok, or X_ERROR_ACCESS_DENIED or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> DeleteContent(u64 xuid,
                                                   const kernel::XCONTENT_AGGREGATE_DATA& data) = 0;

  /**
   * Writes the header file of a package.
   *
   * @param data The content data, title 0xFFFFFFFF for the running title.
   * @return Ok, or X_ERROR_ACCESS_DENIED or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<void> WriteContentHeaderFile(u64 xuid,
                                                            kernel::XCONTENT_AGGREGATE_DATA data,
                                                            u32 license_mask = 0) = 0;

  /**
   * @return The content data, or X_ERROR_FILE_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<kernel::XCONTENT_AGGREGATE_DATA> ReadContentHeaderFile(
      std::string_view file_name, u64 xuid, u32 title_id,
      rex::kernel::XContentType content_type) const = 0;

  /** Returns the folder of the running title's user content for the signed in profile. */
  [[nodiscard]] virtual std::filesystem::path ResolveGameUserContentPath() = 0;

  [[nodiscard]] virtual bool IsContentOpen(const kernel::XCONTENT_AGGREGATE_DATA& data) const = 0;

  /**
   * Closes every open file under a root name.
   */
  virtual void CloseOpenedFilesFromContent(std::string_view root_name) = 0;

  /**
   * Reads the header of a package. Nothing is extracted or written.
   *
   * @param package_path The package file.
   * @return The header facts, kNotFound when the file does not open, or kCorrupt when it is not a
   *         content package, a disc image included.
   */
  [[nodiscard]] virtual Result<PackageInfo> InspectPackage(
      const std::filesystem::path& package_path) const = 0;

  /**
   * Installs a package into the content tree and writes its header file and package manifest. The
   * content type and title ID come from the package metadata. An in place install leaves the
   * original file where it is and mounts it each time the title opens the package.
   *
   * @param package_path The package file.
   * @param options Where the package may come from and whether it is extracted.
   * @return The content data written to the header file, kExists when the package is open, kCorrupt
   *         for a bad image, kUnsupported for an in place SVOD package, a validation error for a
   *         package of another title, or the Container::Open or Extract error.
   */
  [[nodiscard]] virtual Result<kernel::XCONTENT_AGGREGATE_DATA> InstallContent(
      const std::filesystem::path& package_path, const InstallOptions& options = {}) = 0;

  /**
   * Enables or disables an installed package. A disabled package is absent from ListContent,
   * HasContent and OpenContent. The change applies when the title next lists or opens content. A
   * profile flag wins over the all profiles flag, and an installed package with neither is enabled.
   *
   * @param data The content data, title 0xFFFFFFFF for the running title.
   * @param enabled The new flag.
   * @param xuid The profile the flag is for, empty for all profiles.
   * @return Ok, or X_ERROR_FILE_NOT_FOUND when the package is not installed.
   */
  [[nodiscard]] virtual Result<void> SetContentEnabled(const kernel::XCONTENT_AGGREGATE_DATA& data,
                                                       bool enabled,
                                                       std::optional<u64> xuid = std::nullopt) = 0;

  /**
   * Lists the packages installed through InstallContent, disabled ones included.
   *
   * @param title_id The title, 0xFFFFFFFF for the running title.
   */
  [[nodiscard]] virtual std::vector<InstalledPackage> ListInstalledPackages(
      u32 title_id = kernel::kRunningTitleID) const = 0;

  /**
   * Registers a function that runs after an install, a delete or an enable change, on the thread
   * that made the change.
   *
   * @param callback The function. It may call the content manager.
   * @return The guard that removes the function. It must not outlive the content manager.
   */
  [[nodiscard]] virtual rex::ScopeGuard OnContentChanged(
      rex::MoveOnlyFunction<void()> callback) = 0;

  /**
   * Returns the device a package is stored on, or X_ERROR_FILE_NOT_FOUND when no package is open
   * under the name.
   *
   * @param root_name The root name of an open package.
   */
  [[nodiscard]] virtual Result<u32> GetContentDeviceID(std::string_view root_name) const = 0;
};

}  // namespace rex::xam
