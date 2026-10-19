#include <rex/platform/file.h>
#include <rex/codegen/template_registry.h>
#include "template_render.h"

#include <inja/inja.hpp>
#include <rex/hash.h>
#include <rex/logging/log.h>

#include "embedded_templates.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::codegen {

TemplateError::TemplateError(const std::string& template_id, const std::string& message,
                             const std::string& source)
    : std::runtime_error("[" + source + "] template '" + template_id + "': " + message),
      template_id_(template_id),
      source_(source) {}

struct TemplateRegistry::Impl {
  inja::Environment env;
  std::unordered_map<std::string, std::string_view> embedded;
  std::unordered_map<std::string, inja::Template> overrides;
  std::unordered_map<std::string, inja::Template> parsed_cache;

  Impl() {
    embedded = EmbeddedTemplates();

    env.add_callback("cmake_var", 1, [](inja::Arguments& args) {
      return "${" + args.at(0)->get<std::string>() + "}";
    });

    env.add_callback("hex", 1, [](inja::Arguments& args) {
      auto val = args.at(0)->get<u64>();
      std::ostringstream oss;
      oss << "0x" << std::hex << std::uppercase << val;
      return oss.str();
    });

    env.set_search_included_templates_in_files(false);
    env.set_include_callback(
        [this](const std::filesystem::path&, const std::string& name) -> inja::Template {
          std::string id = name;
          if (id.size() > 5 && id.compare(id.size() - 5, 5, ".inja") == 0) {
            id.erase(id.size() - 5);
          }
          auto it = embedded.find(id);
          if (it == embedded.end()) {
            throw TemplateError(name, "include not found in embedded registry");
          }
          return env.parse(std::string(it->second));
        });
  }

  std::string RenderImpl(const std::string& id, const nlohmann::json& data) {
    auto ov_it = overrides.find(id);
    if (ov_it != overrides.end()) {
      return env.render(ov_it->second, data);
    }

    auto cache_it = parsed_cache.find(id);
    if (cache_it != parsed_cache.end()) {
      return env.render(cache_it->second, data);
    }

    auto emb_it = embedded.find(id);
    if (emb_it == embedded.end()) {
      throw TemplateError(id, "no template registered with this ID");
    }

    auto tmpl = env.parse(std::string(emb_it->second));
    auto [it, _] = parsed_cache.emplace(id, std::move(tmpl));
    return env.render(it->second, data);
  }
};

TemplateRegistry::TemplateRegistry() : impl_(std::make_unique<Impl>()) {}
TemplateRegistry::~TemplateRegistry() = default;
TemplateRegistry::TemplateRegistry(TemplateRegistry&&) noexcept = default;
TemplateRegistry& TemplateRegistry::operator=(TemplateRegistry&&) noexcept = default;

void TemplateRegistry::LoadOverrides(const std::filesystem::path& dir) {
  if (!std::filesystem::exists(dir)) {
    REXLOG_WARN(codegen, "Override directory does not exist: {}", dir.string());
    return;
  }

  for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".inja")
      continue;

    auto rel_path = std::filesystem::relative(entry.path(), dir).string();
    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
    auto id = rel_path.substr(0, rel_path.size() - 5);

    if (impl_->embedded.find(id) == impl_->embedded.end()) {
      REXLOG_WARN(codegen, "Override file does not match a known template ID: {} (from {})", id,
                  entry.path().string());
      continue;
    }

    try {
      const auto bytes = rex::platform::ReadHostFile(entry.path());
      const std::string content = bytes ? std::string(bytes->begin(), bytes->end()) : std::string{};
      auto tmpl = impl_->env.parse(content);
      impl_->overrides[id] = std::move(tmpl);
      REXLOG_TRACE(codegen, "Loaded template override: {} -> {}", id, entry.path().string());
    } catch (const std::exception& e) {
      throw TemplateError(id, e.what(), entry.path().string());
    }
  }
}

std::string TemplateRegistry::Render(const std::string& id, const std::string& json_data) {
  try {
    auto data = nlohmann::json::parse(json_data);
    return impl_->RenderImpl(id, data);
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

std::string TemplateRegistry::RenderString(const std::string& template_content,
                                           const std::string& json_data) {
  try {
    auto data = nlohmann::json::parse(json_data);
    auto tmpl = impl_->env.parse(template_content);
    return impl_->env.render(tmpl, data);
  } catch (const nlohmann::json::exception& e) {
    throw TemplateError("<inline>", std::string("JSON parse error: ") + e.what(), "string");
  } catch (const inja::InjaError& e) {
    throw TemplateError("<inline>", e.what(), "string");
  } catch (const std::exception& e) {
    throw TemplateError("<inline>", e.what(), "string");
  }
}

std::vector<std::string> TemplateRegistry::GetRegisteredIds() const {
  std::vector<std::string> ids;
  ids.reserve(impl_->embedded.size());
  for (const auto& [id, _] : impl_->embedded) {
    ids.push_back(id);
  }
  return ids;
}

std::string GetEmbeddedTemplatesHash() {
  static const std::string kHash = [] {
    const auto& templates = EmbeddedTemplates();
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
    return rex::HashBytes(accumulator);
  }();
  return kHash;
}

std::string RenderWithJSON(TemplateRegistry& registry, const std::string& id,
                           const nlohmann::json& data) {
  return registry.Render(id, data.dump());
}

}  // namespace rex::codegen
