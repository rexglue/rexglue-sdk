#include "config_keys.h"

#include <algorithm>
#include <array>
#include <span>

#include <format>

namespace rex::codegen {

namespace {

enum class Type { kString, kInteger, kBoolean, kArray, kTable };

struct KeySpec {
  std::string_view name;
  Type type;
};

constexpr std::array kProjectKeys = {
    KeySpec{"name", Type::kString},
    KeySpec{"sdk_version", Type::kString},
    KeySpec{"game_root", Type::kString},
    KeySpec{"hook_sources", Type::kArray},
};

constexpr std::array kBinaryKeys = {
    KeySpec{"includes", Type::kArray},
    KeySpec{"guest_path", Type::kString},
    KeySpec{"project_name", Type::kString},
    KeySpec{"file_path", Type::kString},
    KeySpec{"out_directory_path", Type::kString},
    KeySpec{"template_dir", Type::kString},
    KeySpec{"longjmp_address", Type::kInteger},
    KeySpec{"setjmp_address", Type::kInteger},
    KeySpec{"skip_lr", Type::kBoolean},
    KeySpec{"skip_msr", Type::kBoolean},
    KeySpec{"ctr_as_local", Type::kBoolean},
    KeySpec{"xer_as_local", Type::kBoolean},
    KeySpec{"reserved_as_local", Type::kBoolean},
    KeySpec{"cr_as_local", Type::kBoolean},
    KeySpec{"non_argument_as_local", Type::kBoolean},
    KeySpec{"non_volatile_as_local", Type::kBoolean},
    KeySpec{"forward_stack_stores", Type::kBoolean},
    KeySpec{"generate_exception_handlers", Type::kBoolean},
    KeySpec{"is_dll", Type::kBoolean},
    KeySpec{"analysis", Type::kTable},
    KeySpec{"shim", Type::kTable},
    KeySpec{"functions", Type::kTable},
    KeySpec{"invalid_instructions", Type::kArray},
    KeySpec{"switch_tables", Type::kArray},
    KeySpec{"midasm_hook", Type::kArray},
    KeySpec{"mmio", Type::kTable},
    KeySpec{"vector_math", Type::kTable},
};

constexpr std::array kVectorMathKeys = {
    KeySpec{"vmaddfp", Type::kString},     KeySpec{"vnmsubfp", Type::kString},
    KeySpec{"vmsum3fp128", Type::kString}, KeySpec{"vmsum4fp128", Type::kString},
    KeySpec{"vrefp", Type::kString},       KeySpec{"vrsqrtefp", Type::kString},
    KeySpec{"vexptefp", Type::kString},    KeySpec{"fres", Type::kString},
    KeySpec{"frsqrte", Type::kString},
};

constexpr std::array kAnalysisKeys = {
    KeySpec{"data_region_threshold", Type::kInteger},
    KeySpec{"exception_handler_funcs", Type::kArray},
};

constexpr std::array kFunctionKeys = {
    KeySpec{"size", Type::kInteger},
    KeySpec{"end", Type::kInteger},
    KeySpec{"name", Type::kString},
    KeySpec{"parent", Type::kInteger},
    KeySpec{"share_registers", Type::kBoolean},
};

constexpr std::array kInvalidInstructionKeys = {
    KeySpec{"data", Type::kInteger},
    KeySpec{"size", Type::kInteger},
};

constexpr std::array kSwitchTableKeys = {
    KeySpec{"address", Type::kInteger},
    KeySpec{"register", Type::kInteger},
    KeySpec{"labels", Type::kArray},
};

constexpr std::array kMidAsmHookKeys = {
    KeySpec{"address", Type::kInteger},
    KeySpec{"name", Type::kString},
    KeySpec{"registers", Type::kArray},
    KeySpec{"return", Type::kBoolean},
    KeySpec{"return_on_true", Type::kBoolean},
    KeySpec{"return_on_false", Type::kBoolean},
    KeySpec{"jump_address", Type::kInteger},
    KeySpec{"jump_address_on_true", Type::kInteger},
    KeySpec{"jump_address_on_false", Type::kInteger},
    KeySpec{"after_instruction", Type::kBoolean},
};

constexpr std::array kMMIOKeys = {
    KeySpec{"gpu", Type::kTable},
    KeySpec{"apu", Type::kTable},
};

constexpr std::array kMMIOBlockKeys = {
    KeySpec{"read", Type::kArray},
    KeySpec{"write", Type::kArray},
};

std::string_view TypeName(Type type) {
  switch (type) {
    case Type::kString:
      return "a string";
    case Type::kInteger:
      return "an integer";
    case Type::kBoolean:
      return "a boolean";
    case Type::kArray:
      return "an array";
    case Type::kTable:
      return "a table";
  }
  return "";
}

bool Matches(const toml::node& node, Type type) {
  switch (type) {
    case Type::kString:
      return node.is_string();
    case Type::kInteger:
      return node.is_integer();
    case Type::kBoolean:
      return node.is_boolean();
    case Type::kArray:
      return node.is_array();
    case Type::kTable:
      return node.is_table();
  }
  return false;
}

std::string Join(std::string_view path, std::string_view key) {
  return path.empty() ? std::string(key) : std::format("{}.{}", path, key);
}

/** Warns about each key of one table that the loader does not read or reads with another type. */
void CheckTable(const toml::table& tbl, std::span<const KeySpec> specs, std::string_view path,
                std::string_view source, std::vector<std::string>& warnings) {
  for (const auto& [key, node] : tbl) {
    const std::string name(key.str());
    const auto spec = std::ranges::find(specs, name, &KeySpec::name);
    if (spec == specs.end()) {
      warnings.push_back(std::format("{}: unknown key '{}'", source, Join(path, name)));
    } else if (!Matches(node, spec->type)) {
      warnings.push_back(
          std::format("{}: key '{}' should be {}", source, Join(path, name), TypeName(spec->type)));
    }
  }
}

void CheckEntries(const toml::node* node, std::span<const KeySpec> specs, std::string_view path,
                  std::string_view source, std::vector<std::string>& warnings) {
  const auto* entries = node ? node->as_array() : nullptr;
  if (!entries) {
    return;
  }
  size_t index = 0;
  for (const auto& entry : *entries) {
    if (const auto* tbl = entry.as_table()) {
      CheckTable(*tbl, specs, std::format("{}[{}]", path, index), source, warnings);
    }
    ++index;
  }
}

}  // namespace

void CheckConfigKeys(const toml::table& root, std::string_view source,
                     std::vector<std::string>& warnings) {
  CheckTable(root, kBinaryKeys, "", source, warnings);

  if (const auto* analysis = root["analysis"].as_table()) {
    CheckTable(*analysis, kAnalysisKeys, "analysis", source, warnings);
  }
  if (const auto* vector_math = root["vector_math"].as_table()) {
    CheckTable(*vector_math, kVectorMathKeys, "vector_math", source, warnings);
  }
  if (const auto* shim = root["shim"].as_table()) {
    for (const auto& [name, node] : *shim) {
      if (!node.is_integer()) {
        warnings.push_back(
            std::format("{}: key 'shim.{}' should be an integer", source, std::string(name.str())));
      }
    }
  }
  if (const auto* functions = root["functions"].as_table()) {
    for (const auto& [address, node] : *functions) {
      if (const auto* entry = node.as_table()) {
        CheckTable(*entry, kFunctionKeys, Join("functions", address.str()), source, warnings);
      }
    }
  }
  CheckEntries(root.get("invalid_instructions"), kInvalidInstructionKeys, "invalid_instructions",
               source, warnings);
  CheckEntries(root.get("switch_tables"), kSwitchTableKeys, "switch_tables", source, warnings);
  CheckEntries(root.get("midasm_hook"), kMidAsmHookKeys, "midasm_hook", source, warnings);
  if (const auto* mmio = root["mmio"].as_table()) {
    CheckTable(*mmio, kMMIOKeys, "mmio", source, warnings);
    for (const auto* block : {"gpu", "apu"}) {
      if (const auto* list = (*mmio)[block].as_table()) {
        CheckTable(*list, kMMIOBlockKeys, Join("mmio", block), source, warnings);
      }
    }
  }
}

void CheckProjectKeys(const toml::table& project, std::string_view source,
                      std::vector<std::string>& warnings) {
  CheckTable(project, kProjectKeys, "", source, warnings);
}

}  // namespace rex::codegen
