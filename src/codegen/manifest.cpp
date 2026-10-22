#include <rex/codegen/manifest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <rex/string/utf8.h>

namespace rex::codegen {

std::string CanonicalizeModuleGuestPath(std::string_view path, std::string_view project_name) {
  std::string guest_path = rex::NormalizeGuestPath(path);

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
 * Loads a binary's recompiler config from its table and sets the project name on it. The other
 * fields come from RecompilerConfig::LoadFromTable.
 *
 * @param tbl The TOML table of the entrypoint or module.
 * @param base_dir The directory that relative paths in the table resolve against.
 * @param project_name The project name stored on the recompiler config.
 * @param shared The shared layer whose defaults the binary starts from.
 * @param out The binary config to fill.
 * @return True when the table loaded, false on failure.
 */
bool LoadBinaryConfig(const toml::table& tbl, const std::filesystem::path& base_dir,
                      std::string_view project_name, const RecompilerConfig& shared,
                      BinaryConfig& out) {
  out.recompiler = shared;
  out.recompiler.ClearAddressEntries();
  out.recompiler.project_name = std::string(project_name);
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
  manifest.manifest_dir = path.parent_path();
  manifest.manifest_path = path;

  auto* project = tbl["project"].as_table();
  if (!project) {
    REX_FAIL(codegen, std::nullopt, "Manifest missing [project] section: {}", path.string());
  }
  manifest.project_name = (*project)["name"].value_or<std::string>("");
  if (manifest.project_name.empty()) {
    REX_FAIL(codegen, std::nullopt, "Manifest missing [project].name: {}", path.string());
  }
  if (!IsValidProjectName(manifest.project_name)) {
    REX_FAIL(codegen, std::nullopt,
             "Manifest [project].name '{}' is not a valid identifier "
             "(letters, digits and underscore, and must not start with a digit): {}",
             manifest.project_name, path.string());
  }
  if (auto stamp = (*project)["sdk_version"].value<std::string>(); stamp && !stamp->empty()) {
    manifest.sdk_version = *stamp;
  }
  if (auto root = (*project)["game_root"].value<std::string>(); root && !root->empty()) {
    manifest.game_root = *root;
  }
  if (auto* sources = (*project)["hook_sources"].as_array()) {
    for (const auto& source : *sources) {
      auto folder = source.value<std::string>();
      if (!folder || folder->empty()) {
        REX_FAIL(codegen, std::nullopt,
                 "Manifest [project].hook_sources must hold non empty folder strings: {}",
                 path.string());
      }
      manifest.hook_sources.push_back(*folder);
    }
  } else if ((*project).contains("hook_sources")) {
    REX_FAIL(codegen, std::nullopt, "Manifest [project].hook_sources must be an array: {}",
             path.string());
  }

  auto* entrypoint = tbl["entrypoint"].as_table();
  if (!entrypoint) {
    REX_FAIL(codegen, std::nullopt, "Manifest missing [entrypoint] section: {}", path.string());
  }
  toml::table shared_table = tbl;
  for (const char* key : {"project", "entrypoint", "modules"}) {
    shared_table.erase(key);
  }
  if (!manifest.shared.LoadShared(shared_table, manifest.manifest_dir)) {
    return std::nullopt;
  }

  if (!LoadBinaryConfig(*entrypoint, manifest.manifest_dir, manifest.project_name, manifest.shared,
                        manifest.entrypoint)) {
    return std::nullopt;
  }

  if (auto modules = tbl["modules"].as_array()) {
    size_t index = 0;
    for (const auto& mod : *modules) {
      auto* mod_tbl = mod.as_table();
      if (!mod_tbl) {
        REX_FAIL(codegen, std::nullopt, "Manifest [[modules]] entry #{} is not a table", index);
      }
      BinaryConfig binary;
      if (!LoadBinaryConfig(*mod_tbl, manifest.manifest_dir, manifest.project_name, manifest.shared,
                            binary)) {
        return std::nullopt;
      }
      auto guest_path = (*mod_tbl)[kModuleGuestPathKey].value_or<std::string>("");
      if (guest_path.empty()) {
        REX_FAIL(codegen, std::nullopt, "Manifest [[modules]] entry #{} missing {}", index,
                 kModuleGuestPathKey);
      }
      binary.guest_path = CanonicalizeModuleGuestPath(guest_path, manifest.project_name);
      for (const auto& existing : manifest.modules) {
        if (existing.guest_path == binary.guest_path) {
          REX_FAIL(codegen, std::nullopt, "Manifest [[modules]] duplicate {} '{}' (entry #{})",
                   kModuleGuestPathKey, binary.guest_path, index);
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

bool ManifestConfig::WriteSDKVersionStamp(const std::filesystem::path& path,
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
    lines.insert(std::next(lines.begin(), static_cast<std::ptrdiff_t>(*project_header_idx + 1)),
                 stamp_line);
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
  auto written = rex::platform::WriteHostFile(
      path, std::span(reinterpret_cast<const u8*>(content.data()), content.size()));
  if (!written) {
    REX_FAIL(codegen, false, "Failed to write the manifest: {}", written.error().message);
  }
  return true;
}

}  // namespace rex::codegen
