#include <fstream>
#include <format>

#include <toml++/toml.hpp>

#include "base/settings/settings_internal.h"

namespace rex {

namespace {

toml::table& TableFor(toml::table& root, std::string_view dotted_scope) {
  toml::table* table = &root;
  size_t start = 0;
  while (start <= dotted_scope.size()) {
    size_t dot = dotted_scope.find('.', start);
    std::string part(dotted_scope.substr(start, dot == std::string_view::npos ? dot : dot - start));
    auto* child = table->get_as<toml::table>(part);
    if (!child) {
      table->insert_or_assign(part, toml::table());
      child = table->get_as<toml::table>(part);
    }
    table = child;
    if (dot == std::string_view::npos)
      break;
    start = dot + 1;
  }
  return *table;
}

Result<void> CollectLeaves(const toml::table& table, const std::string& prefix,
                           const std::filesystem::path& file,
                           std::vector<std::pair<std::string, std::string>>& out) {
  for (auto&& [key, node] : table) {
    std::string name =
        prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
    if (auto* child = node.as_table()) {
      REX_TRY_VOID(CollectLeaves(*child, name, file, out));
    } else if (node.is_array()) {
      return Err(ErrorCategory::kConfig,
                 std::format("{}: key '{}' is an array", file.string(), name));
    } else if (auto* b = node.as_boolean()) {
      out.emplace_back(name, b->get() ? "true" : "false");
    } else if (auto* i = node.as_integer()) {
      out.emplace_back(name, std::to_string(i->get()));
    } else if (auto* f = node.as_floating_point()) {
      out.emplace_back(name, detail::RenderSetting(f->get()));
    } else if (auto* s = node.as_string()) {
      out.emplace_back(name, s->get());
    }
  }
  return Ok();
}

}  // namespace

Result<void> Settings::Save(const std::filesystem::path& file) const {
  toml::table root;
  {
    std::lock_guard lock(state_->mutex);
    for (auto& [name, setting] : state_->settings) {
      if (setting->GetConstraints().no_save || setting->GetText() == setting->GetDefaultText()) {
        continue;
      }
      const size_t dot = name.rfind('.');
      toml::table& table = TableFor(root, std::string_view(name).substr(0, dot));
      const std::string key = name.substr(dot + 1);
      const std::string text = setting->GetText();
      switch (setting->GetType()) {
        case SettingType::kBool:
          table.insert_or_assign(key, text == "true");
          break;
        case SettingType::kI32:
        case SettingType::kI64:
          table.insert_or_assign(key, static_cast<i64>(std::stoll(text)));
          break;
        case SettingType::kU32:
        case SettingType::kU64:
          table.insert_or_assign(key, static_cast<i64>(std::stoull(text)));
          break;
        case SettingType::kF64:
          table.insert_or_assign(key, std::stod(text));
          break;
        default:
          table.insert_or_assign(key, text);
          break;
      }
    }
  }
  std::error_code ec;
  if (file.has_parent_path())
    std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCategory::kIO, std::format("cannot write {}", file.string()));
  }
  out << root << "\n";
  out.flush();
  if (!out) {
    return Err(ErrorCategory::kIO, std::format("cannot write {}", file.string()));
  }
  return Ok();
}

Result<void> Settings::Load(const std::filesystem::path& file, SettingLayer layer) {
  toml::table parsed;
  try {
    parsed = toml::parse_file(file.string());
  } catch (const toml::parse_error& error) {
    return Err(ErrorCategory::kConfig,
               std::format("{}: {}", file.string(), std::string(error.description())));
  }
  std::vector<std::pair<std::string, std::string>> leaves;
  REX_TRY_VOID(CollectLeaves(parsed, {}, file, leaves));
  for (auto& [name, text] : leaves) {
    if (auto r = SetText(layer, name, text); !r) {
      return Err(ErrorCategory::kConfig,
                 std::format("{}: {}", file.string(), r.error().GetDescription()));
    }
  }
  return Ok();
}

}  // namespace rex
