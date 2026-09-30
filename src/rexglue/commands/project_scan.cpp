/**
 * @file        rexglue/commands/project_scan.cpp
 * @brief       Project-tree checks that run on every codegen
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "project_scan.h"
#include "template_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>
#include <unordered_set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>

namespace rexglue::cli {

namespace fs = std::filesystem;

namespace {

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool IsSourceExtension(const fs::path& p) {
  static const std::unordered_set<std::string> kExts = {".cpp", ".cc",  ".cxx", ".c",  ".h",
                                                        ".hh",  ".hpp", ".hxx", ".inl"};
  return kExts.contains(ToLower(p.extension().string()));
}

}  // namespace

std::string RenderRexglueCmake(std::string_view project_name, std::string_view sdk_version,
                               std::string_view entrypoint_out_dir) {
  rex::codegen::TemplateRegistry registry;
  auto names = parse_app_name(std::string(project_name));
  nlohmann::json data = {
      {"names", names_to_json(names)},
      {"sdk_version", std::string(sdk_version)},
      {"entrypoint_out_dir", std::string(entrypoint_out_dir)},
  };
  return registry.render("init/rexglue_cmake", data.dump());
}

bool RefreshGeneratedGlue(const fs::path& project_root, std::string_view project_name,
                          std::string_view sdk_version, std::string_view entrypoint_out_dir) {
  fs::path target = project_root / "generated" / "rexglue.cmake";
  std::string rendered = RenderRexglueCmake(project_name, sdk_version, entrypoint_out_dir);

  std::error_code ec;
  if (fs::exists(target, ec) && read_file(target) == rendered) {
    return false;
  }

  fs::create_directories(target.parent_path(), ec);
  if (!write_file_atomic(target, rendered)) {
    REX_FAIL(rexglue, false, "Failed to regenerate {}", target.string());
  }
  return true;
}

std::vector<SourceWarning> ScanStaleIncludes(
    const fs::path& src_dir, const std::unordered_set<std::string>& removed_basenames) {
  std::vector<SourceWarning> matches;
  if (!fs::is_directory(src_dir))
    return matches;

  std::unordered_set<std::string> removed_lower;
  removed_lower.reserve(removed_basenames.size());
  for (const auto& name : removed_basenames)
    removed_lower.insert(ToLower(name));

  static const std::regex include_re(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");

  for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file() || !IsSourceExtension(entry.path()))
      continue;
    std::ifstream in(entry.path());
    if (!in)
      continue;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
      ++line_number;
      std::smatch m;
      if (!std::regex_search(line, m, include_re))
        continue;
      std::string target = m[1].str();
      auto pos = target.find_last_of("/\\");
      std::string basename = pos == std::string::npos ? target : target.substr(pos + 1);
      if (!removed_lower.contains(ToLower(basename)))
        continue;
      matches.push_back({entry.path(), line_number, fmt::format("stale include: {}", target),
                         "header no longer emitted by codegen; update or remove the include"});
    }
  }

  std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
    if (a.file != b.file)
      return a.file < b.file;
    return a.line_number < b.line_number;
  });
  return matches;
}

}  // namespace rexglue::cli
