/**
 * @file        codegen/template_registry.cpp
 * @brief       Template registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/codegen/template_registry.h>
#include "template_registry_internal.h"

#include <inja/inja.hpp>
#include <rex/logging.h>

#include "codegen_logging.h"

// Generated at configure time by cmake/embed_templates.cmake
#include "embedded_templates.h"

#include <algorithm>
#include <unordered_map>

namespace rex::codegen {

// ---------------------------------------------------------------------------
// TemplateError
// ---------------------------------------------------------------------------

TemplateError::TemplateError(const std::string& templateId, const std::string& message,
                             const std::string& source)
    : std::runtime_error("[" + source + "] template '" + templateId + "': " + message),
      templateId_(templateId),
      source_(source) {}

// ---------------------------------------------------------------------------
// TemplateRegistry::Impl
// ---------------------------------------------------------------------------

struct TemplateRegistry::Impl {
  inja::Environment env_;
  std::unordered_map<std::string, std::string_view> embedded_;
  std::unordered_map<std::string, inja::Template> overrides_;
  std::unordered_map<std::string, inja::Template> parsedCache_;

  Impl() {
    embedded_ = embeddedTemplates();

    // cmake_var callback: wraps a variable name in ${ }
    env_.add_callback("cmake_var", 1, [](inja::Arguments& args) {
      return "${" + args.at(0)->get<std::string>() + "}";
    });

    // hex callback: format an integer as 0x-prefixed hex
    env_.add_callback("hex", 1, [](inja::Arguments& args) {
      auto val = args.at(0)->get<uint64_t>();
      std::ostringstream oss;
      oss << "0x" << std::hex << std::uppercase << val;
      return oss.str();
    });
  }

  std::string renderImpl(const std::string& id, const nlohmann::json& data) {
    // Check overrides first
    auto ovIt = overrides_.find(id);
    if (ovIt != overrides_.end()) {
      return env_.render(ovIt->second, data);
    }

    // Check parsed cache
    auto cacheIt = parsedCache_.find(id);
    if (cacheIt != parsedCache_.end()) {
      return env_.render(cacheIt->second, data);
    }

    // Look up in embedded templates
    auto embIt = embedded_.find(id);
    if (embIt == embedded_.end()) {
      throw TemplateError(id, "no template registered with this ID");
    }

    // Parse, cache, and render
    auto tmpl = env_.parse(std::string(embIt->second));
    auto [it, _] = parsedCache_.emplace(id, std::move(tmpl));
    return env_.render(it->second, data);
  }
};

// ---------------------------------------------------------------------------
// TemplateRegistry special members
// ---------------------------------------------------------------------------

TemplateRegistry::TemplateRegistry() : impl_(std::make_unique<Impl>()) {}
TemplateRegistry::~TemplateRegistry() = default;
TemplateRegistry::TemplateRegistry(TemplateRegistry&&) noexcept = default;
TemplateRegistry& TemplateRegistry::operator=(TemplateRegistry&&) noexcept = default;

// ---------------------------------------------------------------------------
// TemplateRegistry public methods
// ---------------------------------------------------------------------------

void TemplateRegistry::loadOverrides(const std::filesystem::path& dir) {
  if (!std::filesystem::exists(dir)) {
    REXCODEGEN_WARN("Override directory does not exist: {}", dir.string());
    return;
  }

  for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".inja")
      continue;

    // Compute canonical ID from relative path, stripping .inja extension
    auto relPath = std::filesystem::relative(entry.path(), dir).string();
    // Normalize path separators to forward slash
    std::replace(relPath.begin(), relPath.end(), '\\', '/');
    // Strip .inja extension
    auto id = relPath.substr(0, relPath.size() - 5);  // strlen(".inja") == 5

    if (impl_->embedded_.find(id) == impl_->embedded_.end()) {
      REXCODEGEN_WARN("Override file does not match a known template ID: {} (from {})", id,
                      entry.path().string());
      continue;
    }

    try {
      auto content = [&]() {
        std::ifstream ifs(entry.path());
        return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
      }();
      auto tmpl = impl_->env_.parse(content);
      impl_->overrides_[id] = std::move(tmpl);
      REXCODEGEN_INFO("Loaded template override: {} -> {}", id, entry.path().string());
    } catch (const std::exception& e) {
      throw TemplateError(id, e.what(), entry.path().string());
    }
  }
}

std::string TemplateRegistry::render(const std::string& id, const std::string& jsonData) {
  try {
    auto data = nlohmann::json::parse(jsonData);
    return impl_->renderImpl(id, data);
  } catch (const TemplateError&) {
    throw;  // Already wrapped
  } catch (const nlohmann::json::exception& e) {
    throw TemplateError(id, std::string("JSON parse error: ") + e.what());
  } catch (const inja::InjaError& e) {
    throw TemplateError(id, e.what());
  } catch (const std::exception& e) {
    throw TemplateError(id, e.what());
  }
}

std::string TemplateRegistry::renderString(const std::string& templateContent,
                                           const std::string& jsonData) {
  try {
    auto data = nlohmann::json::parse(jsonData);
    auto tmpl = impl_->env_.parse(templateContent);
    return impl_->env_.render(tmpl, data);
  } catch (const nlohmann::json::exception& e) {
    throw TemplateError("<inline>", std::string("JSON parse error: ") + e.what(), "string");
  } catch (const inja::InjaError& e) {
    throw TemplateError("<inline>", e.what(), "string");
  } catch (const std::exception& e) {
    throw TemplateError("<inline>", e.what(), "string");
  }
}

std::vector<std::string> TemplateRegistry::registeredIds() const {
  std::vector<std::string> ids;
  ids.reserve(impl_->embedded_.size());
  for (const auto& [id, _] : impl_->embedded_) {
    ids.push_back(id);
  }
  return ids;
}

// ---------------------------------------------------------------------------
// Free function: renderWithJson (internal API)
// ---------------------------------------------------------------------------

std::string renderWithJson(TemplateRegistry& registry, const std::string& id,
                           const nlohmann::json& data) {
  return registry.render(id, data.dump());
}

}  // namespace rex::codegen
