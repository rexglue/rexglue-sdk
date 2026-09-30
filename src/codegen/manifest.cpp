/**
 * @file        codegen/manifest.cpp
 * @brief       Manifest TOML parser for multi-binary projects
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/manifest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/logging.h>
#include <rex/platform/file.h>
#include <rex/system/guest_path.h>

namespace rex::codegen {

std::string CanonicalizeModuleGuestPath(std::string_view path, std::string_view project_name) {
  std::string guest_path = rex::system::NormalizeGuestPath(path);

  if (!project_name.empty()) {
    std::string lower_project(project_name);
    std::transform(lower_project.begin(), lower_project.end(), lower_project.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string project_assets_prefix = lower_project + "/assets/";
    if (guest_path.rfind(project_assets_prefix, 0) == 0) {
      guest_path.erase(0, project_assets_prefix.size());
    }
  }

  return guest_path;
}

namespace {

bool IsValidProjectName(std::string_view name) {
  if (name.empty())
    return false;
  if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
    return false;
  }
  return std::all_of(name.begin(), name.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

/**
 * Pull file_path / out_directory_path / project_name onto the recompiler
 * config so downstream consumers can rely on them. Other fields (codegen
 * flags, sub-tables, includes) come from RecompilerConfig::LoadFromTable.
 */
bool LoadBinaryConfig(const toml::table& tbl, const std::filesystem::path& base_dir,
                      std::string_view project_name, BinaryConfig& out) {
  out.recompiler = RecompilerConfig{};
  out.recompiler.projectName = std::string(project_name);
  if (!out.recompiler.LoadFromTable(tbl, base_dir)) {
    return false;
  }
  return true;
}

}  // namespace

std::optional<ManifestConfig> ManifestConfig::Load(const std::filesystem::path& path) {
  toml::table tbl;
  try {
    tbl = toml::parse_file(path.string());
  } catch (const toml::parse_error& err) {
    REX_FAIL(codegen, std::nullopt, "Failed to parse manifest {}: {}", path.string(), err.what());
  }

  ManifestConfig manifest;
  manifest.manifestDir = path.parent_path();
  manifest.manifestPath = path;

  auto* project = tbl["project"].as_table();
  if (!project) {
    REX_FAIL(codegen, std::nullopt, "Manifest missing [project] section: {}", path.string());
  }
  manifest.projectName = (*project)["name"].value_or<std::string>("");
  if (manifest.projectName.empty()) {
    REX_FAIL(codegen, std::nullopt, "Manifest missing [project].name: {}", path.string());
  }
  if (!IsValidProjectName(manifest.projectName)) {
    REX_FAIL(codegen, std::nullopt,
             "Manifest [project].name '{}' is not a valid identifier "
             "(letters, digits, underscore; must not start with a digit): {}",
             manifest.projectName, path.string());
  }
  if (auto stamp = (*project)["sdk_version"].value<std::string>(); stamp && !stamp->empty()) {
    manifest.sdkVersion = *stamp;
  }
  if (auto root = (*project)["game_root"].value<std::string>(); root && !root->empty()) {
    manifest.gameRoot = *root;
  }

  auto* entrypoint = tbl["entrypoint"].as_table();
  if (!entrypoint) {
    REX_FAIL(codegen, std::nullopt, "Manifest missing [entrypoint] section: {}", path.string());
  }
  if (!LoadBinaryConfig(*entrypoint, manifest.manifestDir, manifest.projectName,
                        manifest.entrypoint)) {
    return std::nullopt;
  }

  if (auto modules = tbl["modules"].as_array()) {
    size_t index = 0;
    for (const auto& mod : *modules) {
      auto* modTbl = mod.as_table();
      if (!modTbl) {
        REX_FAIL(codegen, std::nullopt, "Manifest [[modules]] entry #{} is not a table", index);
      }
      BinaryConfig binary;
      if (!LoadBinaryConfig(*modTbl, manifest.manifestDir, manifest.projectName, binary)) {
        return std::nullopt;
      }
      auto guest_path = (*modTbl)["guest_path"].value_or<std::string>("");
      if (guest_path.empty()) {
        REX_FAIL(codegen, std::nullopt, "Manifest [[modules]] entry #{} missing guest_path", index);
      }
      binary.guestPath = CanonicalizeModuleGuestPath(guest_path, manifest.projectName);
      for (const auto& existing : manifest.modules) {
        if (existing.guestPath == binary.guestPath) {
          REX_FAIL(codegen, std::nullopt,
                   "Manifest [[modules]] duplicate guest_path '{}' (entry #{})", binary.guestPath,
                   index);
        }
      }
      manifest.modules.push_back(std::move(binary));
      ++index;
    }
  }

  return manifest;
}

bool ManifestConfig::IsManifest(const std::filesystem::path& path) {
  try {
    auto tbl = toml::parse_file(path.string());
    return tbl.contains("project");
  } catch (const toml::parse_error&) {
    return false;
  }
}

namespace {

std::optional<std::string> ParseSectionHeader(std::string_view line) {
  auto first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return std::nullopt;
  if (line[first] != '[' || (first + 1 < line.size() && line[first + 1] == '['))
    return std::nullopt;
  auto end = line.find(']', first + 1);
  if (end == std::string_view::npos)
    return std::nullopt;
  return std::string(line.substr(first + 1, end - first - 1));
}

bool LineSetsKey(std::string_view line, std::string_view key) {
  auto first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return false;
  if (line.compare(first, key.size(), key) != 0)
    return false;
  auto after = first + key.size();
  while (after < line.size() && (line[after] == ' ' || line[after] == '\t'))
    ++after;
  return after < line.size() && line[after] == '=';
}

}  // namespace

bool ManifestConfig::WriteSdkVersionStamp(const std::filesystem::path& path,
                                          std::string_view version) {
  std::ifstream in(path);
  if (!in) {
    REX_FAIL(codegen, false, "Failed to open manifest for stamping: {}", path.string());
  }
  std::vector<std::string> lines;
  std::string buf;
  while (std::getline(in, buf)) {
    lines.push_back(std::move(buf));
    buf.clear();
  }
  in.close();

  const std::string stamp_line = "sdk_version = \"" + std::string(version) + "\"";

  std::optional<size_t> project_header_idx;
  std::optional<size_t> stamp_idx;
  bool in_project = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    auto sec = ParseSectionHeader(lines[i]);
    if (sec) {
      in_project = (*sec == "project");
      if (in_project) {
        project_header_idx = i;
        stamp_idx.reset();
      }
      continue;
    }
    if (in_project && !stamp_idx && LineSetsKey(lines[i], "sdk_version")) {
      stamp_idx = i;
    }
  }

  if (stamp_idx) {
    lines[*stamp_idx] = stamp_line;
  } else if (project_header_idx) {
    lines.insert(lines.begin() + *project_header_idx + 1, stamp_line);
  } else {
    if (!lines.empty() && !lines.back().empty())
      lines.emplace_back();
    lines.emplace_back("[project]");
    lines.push_back(stamp_line);
  }

  std::string content;
  for (const auto& line : lines) {
    content += line;
    content += '\n';
  }
  auto written = rex::platform::WriteWholeFile(
      path, std::span(reinterpret_cast<const u8*>(content.data()), content.size()));
  if (!written) {
    REX_FAIL(codegen, false, "Failed to write the manifest: {}", written.error().message);
  }
  return true;
}

}  // namespace rex::codegen
