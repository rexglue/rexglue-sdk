#include <rex/codegen/template_registry.h>
#include "template_registry_internal.h"

#include <inja/inja.hpp>
#include <rex/hash.h>
#include <rex/logging.h>

#include "embedded_templates.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::codegen {

TemplateError::TemplateError(const std::string& templateId, const std::string& message,
                             const std::string& source)
    : std::runtime_error("[" + source + "] template '" + templateId + "': " + message),
      templateId_(templateId),
      source_(source) {}

struct TemplateRegistry::Impl {
  inja::Environment env_;
  std::unordered_map<std::string, std::string_view> embedded_;
  std::unordered_map<std::string, inja::Template> overrides_;
  std::unordered_map<std::string, inja::Template> parsedCache_;

  Impl() {
    embedded_ = embeddedTemplates();

    env_.add_callback("cmake_var", 1, [](inja::Arguments& args) {
      return "${" + args.at(0)->get<std::string>() + "}";
    });

    env_.add_callback("hex", 1, [](inja::Arguments& args) {
      auto val = args.at(0)->get<u64>();
      std::ostringstream oss;
      oss << "0x" << std::hex << std::uppercase << val;
      return oss.str();
    });

    // Templates are embedded under IDs without the .inja extension, so include names drop it.
    env_.set_search_included_templates_in_files(false);
    env_.set_include_callback(
        [this](const std::filesystem::path&, const std::string& name) -> inja::Template {
          std::string id = name;
          if (id.size() > 5 && id.compare(id.size() - 5, 5, ".inja") == 0) {
            id.erase(id.size() - 5);
          }
          auto it = embedded_.find(id);
          if (it == embedded_.end()) {
            throw TemplateError(name, "include not found in embedded registry");
          }
          return env_.parse(std::string(it->second));
        });
  }

  std::string renderImpl(const std::string& id, const nlohmann::json& data) {
    auto ovIt = overrides_.find(id);
    if (ovIt != overrides_.end()) {
      return env_.render(ovIt->second, data);
    }

    auto cacheIt = parsedCache_.find(id);
    if (cacheIt != parsedCache_.end()) {
      return env_.render(cacheIt->second, data);
    }

    auto embIt = embedded_.find(id);
    if (embIt == embedded_.end()) {
      throw TemplateError(id, "no template registered with this ID");
    }

    auto tmpl = env_.parse(std::string(embIt->second));
    auto [it, _] = parsedCache_.emplace(id, std::move(tmpl));
    return env_.render(it->second, data);
  }
};

TemplateRegistry::TemplateRegistry() : impl_(std::make_unique<Impl>()) {}
TemplateRegistry::~TemplateRegistry() = default;
TemplateRegistry::TemplateRegistry(TemplateRegistry&&) noexcept = default;
TemplateRegistry& TemplateRegistry::operator=(TemplateRegistry&&) noexcept = default;

void TemplateRegistry::loadOverrides(const std::filesystem::path& dir) {
  if (!std::filesystem::exists(dir)) {
    REXLOG_WARN(codegen, "Override directory does not exist: {}", dir.string());
    return;
  }

  for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".inja")
      continue;

    auto relPath = std::filesystem::relative(entry.path(), dir).string();
    std::replace(relPath.begin(), relPath.end(), '\\', '/');
    auto id = relPath.substr(0, relPath.size() - 5);

    if (impl_->embedded_.find(id) == impl_->embedded_.end()) {
      REXLOG_WARN(codegen, "Override file does not match a known template ID: {} (from {})", id,
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
      REXLOG_TRACE(codegen, "Loaded template override: {} -> {}", id, entry.path().string());
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
    throw;
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

std::string EmbeddedTemplatesHash() {
  static const std::string hash = [] {
    const auto& templates = embeddedTemplates();
    std::vector<std::pair<std::string, std::string_view>> ordered(templates.begin(),
                                                                  templates.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::string accumulator;
    for (const auto& [id, content] : ordered) {
      accumulator += id;
      accumulator += '\n';
      accumulator += content;
      accumulator += '\n';
    }
    return rex::hash_bytes(accumulator);
  }();
  return hash;
}

std::string renderWithJson(TemplateRegistry& registry, const std::string& id,
                           const nlohmann::json& data) {
  return registry.render(id, data.dump());
}

}  // namespace rex::codegen
