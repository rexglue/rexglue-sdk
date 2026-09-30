#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform/file.h>

#include "core/logger.h"

namespace rex::log {

namespace {

std::optional<i32> RunNumber(const std::filesystem::path& file, std::string_view prefix) {
  if (file.extension() != ".log") {
    return std::nullopt;
  }
  const std::string stem = file.stem().string();
  if (!stem.starts_with(prefix)) {
    return std::nullopt;
  }
  std::string_view digits(stem);
  digits.remove_prefix(prefix.size());
  digits = digits.substr(0, digits.find('.'));
  i32 number = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (error != std::errc() || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return number;
}

void PruneFolder(const std::filesystem::path& folder, std::string_view prefix, u64 budget) {
  std::map<i32, std::vector<std::filesystem::path>> runs;
  u64 total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const auto run = RunNumber(entry.path(), prefix);
    if (!run) {
      continue;
    }
    const u64 bytes = entry.file_size(ec);
    if (ec) {
      continue;
    }
    runs[*run].push_back(entry.path());
    total += bytes;
  }
  for (const auto& files : runs | std::views::values) {
    if (total <= budget) {
      break;
    }
    for (const auto& file : files) {
      const u64 bytes = std::filesystem::file_size(file, ec);
      if (!ec && std::filesystem::remove(file, ec)) {
        total -= bytes;
      }
    }
  }
}

std::filesystem::path NextRun(const std::filesystem::path& folder, std::string_view name) {
  const std::string prefix = std::format("{}_", name);
  i32 highest = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (const auto run = RunNumber(entry.path(), prefix)) {
      highest = std::max(highest, *run);
    }
  }
  return folder / std::format("{}_{:03d}.log", name, highest + 1);
}

}  // namespace

std::filesystem::path ResolveLogFile(const LogFileConfig& config) {
  if (!config.enabled) {
    return {};
  }
  if (!config.path.empty()) {
    return config.path;
  }
  const std::filesystem::path folder =
      config.folder.empty() ? platform::GetExecutableFolder() / "logs" : config.folder;
  const std::string name =
      config.name.empty() ? platform::GetExecutablePath().stem().string() : config.name;
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (config.folder_budget > 0) {
    PruneFolder(folder, std::format("{}_", name), config.folder_budget);
  }
  return NextRun(folder, name);
}

}  // namespace rex::log
