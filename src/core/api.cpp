/**
 * @file        core/api.cpp
 * @brief       rex::Init, rex::Shutdown, rex::ParseCommandLine and rex::Api
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/api.h>

#include <string_view>

#include "module_loader.h"

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

}  // namespace

bool Api::IsLoaded(std::string_view module_name) const {
  return loader_->IsLoaded(module_name);
}

void* Api::Get(const char* name, uint32_t version) const {
  return loader_->GetForApi(name, version);
}

Result<Api*> Init(const Config& config) {
  return detail::ModuleLoader::Instance().Init(config);
}

void Shutdown() {
  detail::ModuleLoader::Instance().Shutdown();
}

std::vector<std::string> ParseCommandLine(Config& config, int argc, char** argv) {
  std::vector<std::string> remainder;
  constexpr std::string_view kFlag = "--modules";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == kFlag) {
      if (i + 1 < argc) {
        AppendModules(config, argv[i + 1]);
        ++i;
      } else {
        remainder.emplace_back(arg);
      }
      continue;
    }
    if (arg.starts_with(kFlag) && arg.size() > kFlag.size() && arg[kFlag.size()] == '=') {
      AppendModules(config, arg.substr(kFlag.size() + 1));
      continue;
    }
    remainder.emplace_back(arg);
  }
  return remainder;
}

}  // namespace rex
