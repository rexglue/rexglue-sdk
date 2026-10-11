/**
 * @file        codegen/manifest.h
 * @brief       Manifest TOML parser for multi-binary projects
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
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/config.h>

namespace rex::codegen {

/** Manifest key that holds the guest path of a [[modules]] entry. */
inline constexpr std::string_view kModuleGuestPathKey = "guest_path";

/**
 * Codegen settings for a single binary inside a manifest. The entrypoint
 * uses an empty `guestPath`, and module entries set it to the canonicalized
 * guest-visible path the host runtime resolves against.
 */
struct BinaryConfig {
  BinaryConfig() = default;
  BinaryConfig(const BinaryConfig&) = default;
  BinaryConfig& operator=(const BinaryConfig&) = default;
  BinaryConfig(BinaryConfig&&) noexcept(false) = default;
  BinaryConfig& operator=(BinaryConfig&&) noexcept(false) = default;
  ~BinaryConfig() = default;

  RecompilerConfig recompiler; /**< Codegen settings of the binary, loaded from its TOML table. */
  /** Canonical guest path the host runtime resolves the module against, empty for the entrypoint.
   */
  std::string guestPath;
};

/**
 * Canonicalizes a module guest path. Strips the device, normalizes slashes and case, and strips
 * `<project>/assets/` when a matching project name is given.
 *
 * @param path The module path as the title sees it.
 * @param project_name The project name whose `<project>/assets/` prefix is stripped, or empty.
 * @return The canonical path.
 */
std::string CanonicalizeModuleGuestPath(std::string_view path, std::string_view project_name = {});

/**
 * Parsed manifest TOML. Construct via Load() and treat as read-only after.
 */
struct ManifestConfig {
  ManifestConfig() = default;
  ManifestConfig(const ManifestConfig&) = default;
  ManifestConfig& operator=(const ManifestConfig&) = default;
  ManifestConfig(ManifestConfig&&) noexcept(false) = default;
  ManifestConfig& operator=(ManifestConfig&&) noexcept(false) = default;
  ~ManifestConfig() = default;

  std::string projectName;               /**< Value of `[project].name`, a valid identifier. */
  std::optional<std::string> sdkVersion; /**< Last SDK that ran codegen on this project */
  std::filesystem::path manifestPath;    /**< Path the manifest was loaded from. */
  /** Game asset root relative to manifestDir. Set by `rexglue init` to anchor DLL guest paths. */
  std::optional<std::string> gameRoot;
  /** Directory of the manifest, which relative paths in it resolve against. */
  std::filesystem::path manifestDir;
  BinaryConfig entrypoint;           /**< Settings of the `[entrypoint]` binary. */
  std::vector<BinaryConfig> modules; /**< Settings of each `[[modules]]` binary in file order. */

  /**
   * Loads a manifest TOML file.
   *
   * @param path The manifest file.
   * @return The parsed manifest, or nullopt on parse failure.
   */
  static std::optional<ManifestConfig> Load(const std::filesystem::path& path);

  /**
   * Reports whether `path` parses as a manifest, meaning it has a `[project]` section.
   *
   * @param path The file to test.
   * @return True when the file is a manifest.
   */
  static bool IsManifest(const std::filesystem::path& path);

  /**
   * Inserts or overwrites [project].sdk_version in the manifest file at `path`. The rest of the
   * file is preserved.
   *
   * @param path The manifest file to update.
   * @param version The SDK version to store.
   * @return True when the stamp was written, false on parse or write failure.
   */
  static bool WriteSDKVersionStamp(const std::filesystem::path& path, std::string_view version);
};

}  // namespace rex::codegen
