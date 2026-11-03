#include "project_scan.h"
#include "app_name.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>
#include <unordered_set>

#include <format>
#include <nlohmann/json.hpp>
#include "codegen/template_registry.h"
#include "rexglue/log_categories.h"
#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <rex/string/text.h>

namespace rexglue::cli {

namespace fs = std::filesystem;

namespace {

bool IsSourceExtension(const fs::path& p) {
  static constexpr std::array<std::string_view, 9> kExts = {".cpp", ".cc",  ".cxx", ".c",  ".h",
                                                            ".hh",  ".hpp", ".hxx", ".inl"};
  const std::string ext = p.extension().string();
  return std::ranges::any_of(
      kExts, [&](std::string_view known) { return rex::string::utf8_equal_case(ext, known); });
}

}  // namespace

std::string RenderReXGlueCMake(std::string_view project_name, std::string_view sdk_version,
                               std::string_view entrypoint_out_dir) {
  rex::codegen::TemplateRegistry registry;
  auto names = ParseAppName(std::string(project_name));
  nlohmann::json data = {
      {"names", NamesToJSON(names)},
      {"sdk_version", std::string(sdk_version)},
      {"entrypoint_out_dir", std::string(entrypoint_out_dir)},
  };
  return registry.Render("init/rexglue_cmake", data.dump());
}

bool RefreshGeneratedCMake(const fs::path& project_root, std::string_view project_name,
                           std::string_view sdk_version, std::string_view entrypoint_out_dir) {
  fs::path target = project_root / "generated" / "rexglue.cmake";
  std::string rendered = RenderReXGlueCMake(project_name, sdk_version, entrypoint_out_dir);

  if (auto existing = rex::platform::ReadHostFile(target);
      existing && std::string_view(reinterpret_cast<const char*>(existing->data()),
                                   existing->size()) == rendered) {
    return false;
  }

  if (auto written = rex::platform::WriteHostFile(target, rendered); !written) {
    REX_FAIL(rexglue, false, "Failed to regenerate {}: {}", target.string(),
             written.error().message);
  }
  return true;
}

std::vector<SourceWarning> ScanStaleIncludes(
    const fs::path& src_dir, const std::unordered_set<std::string>& removed_basenames) {
  std::vector<SourceWarning> matches;
  if (!fs::is_directory(src_dir))
    return matches;

  std::unordered_set<rex::string::StringKeyIgnoreCase> removed;
  removed.reserve(removed_basenames.size());
  for (const auto& name : removed_basenames)
    removed.insert(rex::string::StringKeyIgnoreCase::Create(name));

  static const std::regex kIncludeRe(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");

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
      if (!std::regex_search(line, m, kIncludeRe))
        continue;
      std::string target = m[1].str();
      auto pos = target.find_last_of("/\\");
      std::string basename = pos == std::string::npos ? target : target.substr(pos + 1);
      if (!removed.contains(rex::string::StringKeyIgnoreCase::Create(basename)))
        continue;
      matches.push_back({entry.path(), line_number, std::format("stale include: {}", target),
                         "Header no longer emitted by codegen. Update or remove the include."});
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
