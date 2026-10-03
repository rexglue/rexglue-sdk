/**
 * @file        core/api.cpp
 * @brief       rex::Init, rex::Shutdown, rex::API, the command line and the config file helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <string_view>

#include <toml++/toml.hpp>

#include "module_loader.h"

#include "core/cvar_table.h"

namespace rex {

namespace {

void AppendModules(Config& config, std::string_view list) {
  size_t start = 0;
  while (start <= list.size()) {
    size_t comma = list.find(',', start);
    std::string_view item = list.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    if (!item.empty()) {
      config.modules.emplace_back(item);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
}

struct StructuralField {
  std::string_view flag;
  void (*apply)(Config&, std::string_view);
};

constexpr std::array<StructuralField, 8> kStructuralFields = {{
    {"--modules", [](Config& c, std::string_view v) { AppendModules(c, v); }},
    {"--game_data_root", [](Config& c, std::string_view v) { c.paths.game_data_root = v; }},
    {"--user_data_root", [](Config& c, std::string_view v) { c.paths.user_data_root = v; }},
    {"--update_data_root", [](Config& c, std::string_view v) { c.paths.update_data_root = v; }},
    {"--cache_root", [](Config& c, std::string_view v) { c.paths.cache_root = v; }},
    {"--metadata_root", [](Config& c, std::string_view v) { c.paths.metadata_root = v; }},
    {"--config_file", [](Config& c, std::string_view v) { c.paths.config_file = v; }},
    {"--backend", [](Config& c, std::string_view v) { c.gpu.backend = std::string(v); }},
}};

bool LowerToken(std::string_view part) {
  if (part.empty() || !std::islower(static_cast<unsigned char>(part.front())))
    return false;
  return std::ranges::all_of(part, [](char c) {
    return std::islower(static_cast<unsigned char>(c)) ||
           std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

bool IsCvarName(std::string_view name) {
  auto dot = name.find('.');
  if (dot == std::string_view::npos || name.find('.', dot + 1) != std::string_view::npos) {
    return false;
  }
  return LowerToken(name.substr(0, dot)) && LowerToken(name.substr(dot + 1));
}

std::string TOMLScalar(const toml::node& node) {
  if (auto* v = node.as_string())
    return v->get();
  if (auto* v = node.as_boolean())
    return v->get() ? "true" : "false";
  if (auto* v = node.as_integer())
    return std::to_string(v->get());
  if (auto* v = node.as_floating_point())
    return std::format("{}", v->get());
  return {};
}

std::string FirstKey(const toml::node& node) {
  if (auto* table = node.as_table()) {
    for (auto&& [key, unused] : *table)
      return std::string(key.str());
    return "...";
  }
  return "[]";
}

}  // namespace

bool API::IsLoaded(std::string_view module_name) const {
  return loader_->IsLoaded(module_name);
}

void* API::Get(const char* name, u32 version) const {
  return loader_->GetForAPI(name, version);
}

void API::UpdatePaths(const PathsConfig& paths) {
  loader_->UpdatePaths(paths);
}

const Config& API::config() const {
  return loader_->config();
}

Result<API*> Init(const Config& config) {
  return detail::ModuleLoader::Instance().Init(config);
}

void Shutdown() {
  detail::ModuleLoader::Instance().Shutdown();
}

std::vector<std::string> ParseCommandLine(Config& config, const CLIArgList& args) {
  std::vector<std::string> unknown;
  for (const CLIOption& option : args.Options()) {
    const std::string text =
        option.value ? std::format("--{}={}", option.name, *option.value) : "--" + option.name;
    if (auto field = std::ranges::find_if(
            kStructuralFields,
            [&](const StructuralField& entry) { return entry.flag.substr(2) == option.name; });
        field != std::ranges::end(kStructuralFields)) {
      if (!option.value) {
        unknown.push_back(text);
        continue;
      }
      field->apply(config, *option.value);
      continue;
    }
    std::string_view name = option.name;
    const bool negated = name.starts_with("no-");
    if (negated) {
      name.remove_prefix(3);
    }
    if (!IsCvarName(name)) {
      unknown.push_back(text);
      continue;
    }
    const std::string value = negated ? "false" : option.value.value_or("true");
    config.cvars.Set(name, value);
  }
  return unknown;
}

Result<void> LoadConfigFile(Config& config, const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return Ok();
  toml::table parsed;
  try {
    parsed = toml::parse_file(path.string());
  } catch (const toml::parse_error& error) {
    return Err(ErrorCategory::Config,
               std::format("{}: {}", path.string(), std::string(error.description())));
  }
  for (auto&& [key, node] : parsed) {
    std::string top(key.str());
    if (auto* table = node.as_table()) {
      for (auto&& [leaf, value] : *table) {
        if (value.is_table() || value.is_array()) {
          return Err(ErrorCategory::Config,
                     std::format("{}: key '{}.{}.{}' nests deeper than scope.leaf", path.string(),
                                 top, std::string(leaf.str()), FirstKey(value)));
        }
        config.cvars.values.push_back({std::format("{}.{}", top, std::string(leaf.str())),
                                       TOMLScalar(value), cvar::CvarSource::Config});
      }
      continue;
    }
    if (node.is_array()) {
      return Err(ErrorCategory::Config,
                 std::format("{}: key '{}' is an array", path.string(), top));
    }
    config.cvars.values.push_back({top, TOMLScalar(node), cvar::CvarSource::Config});
  }
  return Ok();
}

Result<void> SaveConfigFile(const cvar::ICvarRegistry& registry,
                            const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCategory::Config, std::format("{}: cannot open for writing", path.string()));
  }
  out << "# ReXGlue configuration\n\n" << registry.Serialize();
  if (!out) {
    return Err(ErrorCategory::Config, std::format("{}: write failed", path.string()));
  }
  return Ok();
}

}  // namespace rex

namespace rex::detail {

Result<void> RegisterBuiltinCvars(cvar::ICvarRegistry& registry) {
  REX_TRY_VOID(registry.Register("log", rex::cvars::kLogCvars));
  REX_TRY_VOID(registry.Register("clock", rex::cvars::kClockCvars));
  REX_TRY_VOID(registry.Register("perf", rex::cvars::kPerfCvars));
  return Ok();
}

void UnregisterBuiltinCvars(cvar::ICvarRegistry& registry) {
  registry.Unregister(rex::cvars::kPerfCvars);
  registry.Unregister(rex::cvars::kClockCvars);
  registry.Unregister(rex::cvars::kLogCvars);
}

}  // namespace rex::detail
