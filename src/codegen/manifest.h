/**
 * @file        codegen/manifest.h
 * @brief       Manifest TOML parser for multi binary projects
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

#include "config.h"

namespace rex::codegen {

/** Manifest key that holds the guest path of a [[modules]] entry. */
inline constexpr std::string_view kModuleGuestPathKey = "guest_path";

/** Codegen settings for one binary in a manifest. */
struct BinaryConfig {
  BinaryConfig() = default;
  BinaryConfig(const BinaryConfig&) = default;
  BinaryConfig& operator=(const BinaryConfig&) = default;
  BinaryConfig(BinaryConfig&&) noexcept(false) = default;
  BinaryConfig& operator=(BinaryConfig&&) noexcept(false) = default;
  ~BinaryConfig() = default;

  RecompilerConfig recompiler;
  std::string guest_path; /**< Canonical guest path of the module, empty for the entrypoint. */
};

/**
 * Canonicalizes a module guest path and strips a leading `<project>/assets/`.
 *
 * @param project_name The project name of the prefix to strip, or empty.
 * @return The canonical path.
 */
std::string CanonicalizeModuleGuestPath(std::string_view path, std::string_view project_name = {});

struct ManifestConfig {
  ManifestConfig() = default;
  ManifestConfig(const ManifestConfig&) = default;
  ManifestConfig& operator=(const ManifestConfig&) = default;
  ManifestConfig(ManifestConfig&&) noexcept(false) = default;
  ManifestConfig& operator=(ManifestConfig&&) noexcept(false) = default;
  ~ManifestConfig() = default;

  std::string project_name;               /**< Value of `[project].name`, a valid identifier. */
  std::optional<std::string> sdk_version; /**< Last SDK that ran codegen on this project. */
  std::filesystem::path manifest_path;
  std::optional<std::string> game_root;  /**< Game asset root relative to manifest_dir. */
  std::vector<std::string> hook_sources; /**< Folders relative to manifest_dir scanned for hooks. */
  std::filesystem::path manifest_dir;    /**< Base for relative paths in the manifest. */
  std::vector<std::string> warnings;     /**< Unread or mistyped `[project]` keys. */
  /**
   * Top level keys other than `project`, `entrypoint` and `modules`. Address keyed entries stay
   * here until the images load, then go to the binary containing the address.
   */
  RecompilerConfig shared;
  BinaryConfig entrypoint;
  std::vector<BinaryConfig> modules; /**< In file order. */

  /**
   * Loads a manifest TOML file.
   *
   * @return The parsed manifest, or nullopt on parse failure.
   */
  static std::optional<ManifestConfig> Load(const std::filesystem::path& path);

  /** Reports whether `path` parses as a manifest with a `[project]` section. */
  static bool IsManifest(const std::filesystem::path& path);

  /**
   * Inserts or overwrites [project].sdk_version in the manifest file.
   *
   * @return True when the stamp was written, false on parse or write failure.
   */
  static bool WriteSDKVersionStamp(const std::filesystem::path& path, std::string_view version);
};

}  // namespace rex::codegen
