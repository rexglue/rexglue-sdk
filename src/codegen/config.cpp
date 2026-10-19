#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <vector>

#include <format>

#include <rex/codegen/config.h>
#include <rex/kernel/shim/registry.h>
#include <rex/fail.h>
#include <rex/logging/log.h>

#include <toml++/toml.hpp>

namespace rex::codegen {

namespace {

/** Maximum nesting depth for include chains. */
constexpr u32 kMaxIncludeDepth = 32;

/**
 * Parses a hex address string with or without a "0x" or "0X" prefix.
 *
 * @param key_str Address text.
 * @return The address, or nullopt when the text is not hex.
 */
std::optional<u32> ParseHexAddress(const std::string& key_str) {
  try {
    if (key_str.starts_with("0x") || key_str.starts_with("0X")) {
      return static_cast<u32>(std::stoul(key_str.substr(2), nullptr, 16));
    } else {
      return static_cast<u32>(std::stoul(key_str, nullptr, 16));
    }
  } catch (...) {
    return std::nullopt;
  }
}

template <typename T>
void MergeScalar(T& dst, const T& src, const char* name) {
  if (src != T{} && src != dst) {
    if (dst != T{}) {
      if constexpr (std::is_same_v<T, std::string>) {
        REXLOG_DEBUG(codegen, "config   {}: \"{}\" -> \"{}\" (overridden)", name, dst, src);
      } else {
        REXLOG_DEBUG(codegen, "config   {}: {} -> {} (overridden)", name, dst, src);
      }
    }
    dst = src;
  }
}

/**
 * Merges a boolean where false is a meaningful value, skipped when the key is absent.
 *
 * @param dst Destination value.
 * @param src Overlay value.
 * @param present True when the overlay sets the key.
 * @param name Key name for the override log.
 */
void MergeBool(bool& dst, bool src, bool present, const char* name) {
  if (!present)
    return;
  if (src != dst) {
    REXLOG_DEBUG(codegen, "config   {}: {} -> {} (overridden)", name, dst ? "true" : "false",
                 src ? "true" : "false");
  }
  dst = src;
}

size_t EditDistance(std::string_view a, std::string_view b) {
  std::vector<size_t> row(b.size() + 1);
  std::iota(row.begin(), row.end(), size_t{0});
  for (size_t i = 1; i <= a.size(); ++i) {
    size_t diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

std::string_view NearestShimName(std::string_view name) {
  constexpr size_t kMaxDistance = 2;
  std::string_view best;
  size_t best_distance = kMaxDistance + 1;
  for (const auto& entry : rex::kernel::shim::kRegistry) {
    const size_t distance = EditDistance(name, entry.name);
    if (distance < best_distance) {
      best = entry.name;
      best_distance = distance;
    }
  }
  return best;
}

bool ApplyTOML(const toml::table& root, RecompilerConfig& cfg, const std::string& description) {
  if (auto* retired = root["rexcrt"].as_table()) {
    std::string converted = "[shim]";
    for (const auto& [name, val] : *retired) {
      if (auto addr = val.value<i64>()) {
        converted +=
            std::format("\n{} = 0x{:08X}", std::string(name.str()), static_cast<u32>(*addr));
      }
    }
    REXLOG_ERROR(codegen, "config {}: the [rexcrt] table was renamed, replace it with\n{}",
                 description, converted);
    return false;
  }

  if (auto v = root["project_name"].value<std::string>()) {
    MergeScalar(cfg.project_name, *v, "project_name");
  }
  if (auto v = root["file_path"].value<std::string>()) {
    MergeScalar(cfg.file_path, *v, "file_path");
  }
  if (auto v = root["out_directory_path"].value<std::string>()) {
    MergeScalar(cfg.out_directory_path, *v, "out_directory_path");
  }
  if (auto v = root["template_dir"].value<std::string>()) {
    MergeScalar(cfg.template_dir, *v, "template_dir");
  }

  auto has_bool = [&](const char* key) -> bool { return root[key].is_boolean(); };
  MergeBool(cfg.skip_lr, root["skip_lr"].value_or(false), has_bool("skip_lr"), "skip_lr");
  MergeBool(cfg.skip_msr, root["skip_msr"].value_or(false), has_bool("skip_msr"), "skip_msr");
  MergeBool(cfg.ctr_as_local_variable, root["ctr_as_local"].value_or(false),
            has_bool("ctr_as_local"), "ctr_as_local");
  MergeBool(cfg.xer_as_local_variable, root["xer_as_local"].value_or(false),
            has_bool("xer_as_local"), "xer_as_local");
  MergeBool(cfg.reserved_register_as_local_variable, root["reserved_as_local"].value_or(false),
            has_bool("reserved_as_local"), "reserved_as_local");
  MergeBool(cfg.cr_registers_as_local_variables, root["cr_as_local"].value_or(false),
            has_bool("cr_as_local"), "cr_as_local");
  MergeBool(cfg.non_argument_registers_as_local_variables,
            root["non_argument_as_local"].value_or(false), has_bool("non_argument_as_local"),
            "non_argument_as_local");
  MergeBool(cfg.non_volatile_registers_as_local_variables,
            root["non_volatile_as_local"].value_or(false), has_bool("non_volatile_as_local"),
            "non_volatile_as_local");
  MergeBool(cfg.generate_exception_handlers, root["generate_exception_handlers"].value_or(false),
            has_bool("generate_exception_handlers"), "generate_exception_handlers");
  if (has_bool("is_dll")) {
    cfg.is_dll = root["is_dll"].value_or(false);
  }

  if (auto v = root["longjmp_address"].value<i64>()) {
    u32 addr = static_cast<u32>(*v);
    MergeScalar(cfg.longjmp_address, addr, "longjmp_address");
  }
  if (auto v = root["setjmp_address"].value<i64>()) {
    u32 addr = static_cast<u32>(*v);
    MergeScalar(cfg.setjmp_address, addr, "setjmp_address");
  }

  if (auto* analysis_table = root["analysis"].as_table()) {
    if (auto v = (*analysis_table)["max_jump_extension"].value<u32>()) {
      MergeScalar(cfg.max_jump_extension, *v, "analysis.max_jump_extension");
    }
    if (auto v = (*analysis_table)["data_region_threshold"].value<u32>()) {
      MergeScalar(cfg.data_region_threshold, *v, "analysis.data_region_threshold");
    }
    if (auto v = (*analysis_table)["large_function_threshold"].value<u32>()) {
      MergeScalar(cfg.large_function_threshold, *v, "analysis.large_function_threshold");
    }

    if (auto handlers = (*analysis_table)["exception_handler_funcs"].as_array()) {
      for (const auto& elem : *handlers) {
        if (auto val = elem.value<i64>()) {
          cfg.exception_handler_func_hints.push_back(static_cast<u32>(*val));
        }
      }
    }
  }

  if (auto* shim_table = root["shim"].as_table()) {
    size_t added = 0;
    for (const auto& [name, val] : *shim_table) {
      if (auto addr = val.value<i64>()) {
        auto key = std::string(name);
        auto it = cfg.shim_functions.find(key);
        if (it != cfg.shim_functions.end()) {
          REXLOG_DEBUG(codegen, "config   [shim] {} overridden: 0x{:08X} -> 0x{:08X}", key,
                       it->second, static_cast<u32>(*addr));
        }
        cfg.shim_functions[key] = static_cast<u32>(*addr);
        ++added;
      }
    }
    if (added > 0) {
      REXLOG_DEBUG(codegen, "config   [shim] added/updated {} entries from {}", added, description);
    }
  }

  if (auto functions_table = root["functions"].as_table()) {
    size_t added = 0;
    for (auto& [key, value] : *functions_table) {
      std::string key_str(key.str());
      auto addr_opt = ParseHexAddress(key_str);
      if (!addr_opt) {
        REXLOG_ERROR(codegen, "Invalid function address key: {}", key_str);
        continue;
      }
      u32 address = *addr_opt;

      auto* table = value.as_table();
      if (!table) {
        REXLOG_ERROR(codegen, "Invalid [functions] entry at 0x{:08X}: expected table", address);
        continue;
      }

      FunctionConfig fcfg;
      fcfg.size = (*table)["size"].value_or(0u);
      fcfg.end = (*table)["end"].value_or(0u);
      fcfg.name = (*table)["name"].value_or(std::string{});
      fcfg.parent = (*table)["parent"].value_or(0u);
      fcfg.share_registers = (*table)["share_registers"].value_or(false);

      if (fcfg.size && fcfg.end) {
        REXLOG_ERROR(codegen, "Function 0x{:08X}: cannot specify both 'size' and 'end'", address);
        continue;
      }
      if (fcfg.end && fcfg.end <= address) {
        REXLOG_ERROR(codegen, "Function 0x{:08X}: 'end' (0x{:08X}) must be greater than address",
                     address, fcfg.end);
        continue;
      }

      auto it = cfg.functions.find(address);
      if (it != cfg.functions.end()) {
        REXLOG_DEBUG(codegen, "config   [functions] 0x{:08X} overridden from {}", address,
                     description);
      }
      cfg.functions.insert_or_assign(address, std::move(fcfg));
      ++added;
    }
    if (added > 0) {
      REXLOG_DEBUG(codegen, "config   [functions] added/updated {} entries from {}", added,
                   description);
    }
  }

  if (auto invalid_array = root["invalid_instructions"].as_array()) {
    for (auto& entry : *invalid_array) {
      auto* table = entry.as_table();
      if (!table) {
        REXLOG_ERROR(codegen, "Invalid [[invalid_instructions]] entry: expected table");
        continue;
      }

      auto data_opt = (*table)["data"].value<u32>();
      auto size_opt = (*table)["size"].value<u32>();

      if (!data_opt) {
        REXLOG_ERROR(codegen, "Missing 'data' in [[invalid_instructions]] entry");
        continue;
      }
      if (!size_opt) {
        REXLOG_ERROR(codegen, "Missing 'size' in [[invalid_instructions]] entry");
        continue;
      }

      auto it = cfg.invalid_instruction_hints.find(*data_opt);
      if (it != cfg.invalid_instruction_hints.end()) {
        REXLOG_DEBUG(codegen, "config   [[invalid_instructions]] 0x{:08X} overridden from {}",
                     *data_opt, description);
      }
      cfg.invalid_instruction_hints.insert_or_assign(*data_opt, *size_opt);
    }
  }

  if (auto switch_table_array = root["switch_tables"].as_array()) {
    for (auto& entry : *switch_table_array) {
      auto* table = entry.as_table();
      if (!table) {
        REXLOG_ERROR(codegen, "Invalid [[switch_tables]] entry: expected table");
        continue;
      }

      auto address_opt = (*table)["address"].value<u32>();
      auto register_opt = (*table)["register"].value<u32>();
      auto labels_array = (*table)["labels"].as_array();

      if (!address_opt) {
        REXLOG_ERROR(codegen, "Missing 'address' in [[switch_tables]] entry");
        continue;
      }
      if (!register_opt) {
        REXLOG_ERROR(codegen, "Missing 'register' in [[switch_tables]] entry");
        continue;
      }
      if (!labels_array) {
        REXLOG_ERROR(codegen, "Missing 'labels' in [[switch_tables]] entry");
        continue;
      }

      JumpTable jt;
      jt.bctr_address = *address_opt;
      jt.table_address = 0;
      jt.index_register = static_cast<u8>(*register_opt);

      for (auto& label : *labels_array) {
        if (auto label_val = label.value<i64>()) {
          jt.targets.push_back(static_cast<u32>(*label_val));
        }
      }

      if (jt.targets.empty()) {
        REXLOG_ERROR(codegen, "Empty 'labels' array in [[switch_tables]] at 0x{:08X}",
                     *address_opt);
        continue;
      }

      auto it = cfg.switch_tables.find(*address_opt);
      if (it != cfg.switch_tables.end()) {
        REXLOG_DEBUG(codegen, "config   [[switch_tables]] 0x{:08X} overridden from {}",
                     *address_opt, description);
      }
      size_t label_count = jt.targets.size();
      cfg.switch_tables.insert_or_assign(*address_opt, std::move(jt));
      REXLOG_DEBUG(codegen, "Loaded manual switch table at 0x{:08X} with {} labels", *address_opt,
                   label_count);
    }
  }

  if (auto mid_asm_hook_array = root["midasm_hook"].as_array()) {
    for (auto& entry : *mid_asm_hook_array) {
      auto* table = entry.as_table();
      if (!table) {
        REXLOG_ERROR(codegen, "Invalid [[midasm_hook]] entry: expected table");
        continue;
      }

      auto address_opt = (*table)["address"].value<u32>();
      auto name_opt = (*table)["name"].value<std::string>();

      if (!address_opt) {
        REXLOG_ERROR(codegen, "Missing 'address' in [[midasm_hook]] entry");
        continue;
      }
      if (!name_opt) {
        REXLOG_ERROR(codegen, "Missing 'name' in [[midasm_hook]] entry");
        continue;
      }

      MidAsmHook mid_asm_hook;
      mid_asm_hook.name = *name_opt;

      if (auto register_array = (*table)["registers"].as_array()) {
        for (auto& reg : *register_array) {
          if (auto reg_str = reg.value<std::string>()) {
            mid_asm_hook.registers.push_back(*reg_str);
          }
        }
      }

      mid_asm_hook.ret = (*table)["return"].value_or(false);
      mid_asm_hook.return_on_true = (*table)["return_on_true"].value_or(false);
      mid_asm_hook.return_on_false = (*table)["return_on_false"].value_or(false);

      mid_asm_hook.jump_address = (*table)["jump_address"].value_or(0u);
      mid_asm_hook.jump_address_on_true = (*table)["jump_address_on_true"].value_or(0u);
      mid_asm_hook.jump_address_on_false = (*table)["jump_address_on_false"].value_or(0u);

      if ((mid_asm_hook.ret && mid_asm_hook.jump_address != 0) ||
          (mid_asm_hook.return_on_true && mid_asm_hook.jump_address_on_true != 0) ||
          (mid_asm_hook.return_on_false && mid_asm_hook.jump_address_on_false != 0)) {
        REXLOG_ERROR(codegen, "{}: can't return and jump at the same time", mid_asm_hook.name);
      }

      if ((mid_asm_hook.ret || mid_asm_hook.jump_address != 0) &&
          (mid_asm_hook.return_on_false || mid_asm_hook.return_on_true ||
           mid_asm_hook.jump_address_on_false != 0 || mid_asm_hook.jump_address_on_true != 0)) {
        REXLOG_ERROR(codegen, "{}: can't mix direct and conditional return/jump",
                     mid_asm_hook.name);
      }

      mid_asm_hook.after_instruction = (*table)["after_instruction"].value_or(false);

      auto it = cfg.mid_asm_hooks.find(*address_opt);
      if (it != cfg.mid_asm_hooks.end()) {
        REXLOG_DEBUG(codegen, "config   [[midasm_hook]] 0x{:08X} overridden from {}", *address_opt,
                     description);
      }
      cfg.mid_asm_hooks.insert_or_assign(*address_opt, std::move(mid_asm_hook));
    }
  }

  if (auto indirect_call_array = root["indirect_calls"].as_array()) {
    for (auto& entry : *indirect_call_array) {
      if (auto addr = entry.value<i64>()) {
        cfg.known_indirect_call_hints.insert(static_cast<u32>(*addr));
        REXLOG_DEBUG(codegen, "Loaded known indirect call hint at 0x{:08X}", *addr);
      }
    }
  }
  for (const MMIOBlock block : {MMIOBlock::kGPU, MMIOBlock::kAPU}) {
    const std::string_view block_name = block == MMIOBlock::kGPU ? "gpu" : "apu";
    for (const bool write : {false, true}) {
      const std::string_view list_name = write ? "write" : "read";
      const auto* list = root["mmio"][block_name][list_name].as_array();
      if (!list) {
        continue;
      }
      for (const auto& entry : *list) {
        const auto addr = entry.value<i64>();
        if (!addr) {
          REXLOG_ERROR(codegen, "[mmio.{}] {} holds an entry that is not an integer address",
                       block_name, list_name);
          return false;
        }
        if (!cfg.declared_mmio_accesses.try_emplace(static_cast<u32>(*addr), block, write).second) {
          REXLOG_ERROR(codegen, "[mmio.{}] {} 0x{:08X} is declared twice", block_name, list_name,
                       *addr);
          return false;
        }
      }
    }
  }
  return true;
}

bool ApplyTableWithIncludes(const toml::table& tbl, const std::filesystem::path& base_dir,
                            RecompilerConfig& cfg, std::set<std::string>& visited, u32 depth,
                            const std::string& description);

bool LoadRecursive(const std::filesystem::path& file_path, RecompilerConfig& cfg,
                   std::set<std::string>& visited, u32 depth) {
  if (depth > kMaxIncludeDepth) {
    REX_FAIL(codegen, false, "config include depth exceeds maximum ({}) at: {}", kMaxIncludeDepth,
             file_path.string());
  }

  std::error_code ec;
  auto canonical = std::filesystem::canonical(file_path, ec);
  if (ec) {
    REX_FAIL(codegen, false, "config cannot resolve path '{}': {}", file_path.string(),
             ec.message());
  }
  std::string canonical_str = canonical.string();
  if (visited.contains(canonical_str)) {
    REX_FAIL(codegen, false, "config circular include detected: {}", canonical_str);
  }
  visited.insert(canonical_str);
  cfg.loaded_files.push_back(canonical_str);

  toml::table table;
  try {
    table = toml::parse_file(canonical_str);
  } catch (const toml::parse_error& e) {
    REX_FAIL(codegen, false, "Failed to parse config file '{}': {}", canonical_str, e.what());
  }
  REXLOG_DEBUG(codegen, "config loaded: {}", file_path.filename().string());
  return ApplyTableWithIncludes(table, canonical.parent_path(), cfg, visited, depth,
                                file_path.filename().string());
}

bool ApplyTableWithIncludes(const toml::table& tbl, const std::filesystem::path& base_dir,
                            RecompilerConfig& cfg, std::set<std::string>& visited, u32 depth,
                            const std::string& description) {
  // Process includes first (depth-first), so this table's own values win.
  if (auto* includes_array = tbl["includes"].as_array()) {
    for (const auto& elem : *includes_array) {
      if (auto include_path = elem.value<std::string>()) {
        auto resolved = base_dir / *include_path;
        if (!std::filesystem::exists(resolved)) {
          REX_FAIL(codegen, false, "config included file not found: {} (resolved from {})",
                   *include_path, description);
        }
        if (!LoadRecursive(resolved, cfg, visited, depth + 1)) {
          return false;
        }
      }
    }
  }
  return ApplyTOML(tbl, cfg, description);
}

}  // namespace

namespace {

bool FinalizeConfig(RecompilerConfig& cfg) {
  if (!cfg.functions.empty()) {
    size_t chunks_count = 0;
    for (const auto& [addr, fc] : cfg.functions) {
      if (fc.IsChunk())
        chunks_count++;
    }
    REXLOG_TRACE(codegen, "Loaded {} function configs ({} standalone, {} chunks)",
                 cfg.functions.size(), cfg.functions.size() - chunks_count, chunks_count);
  }
  if (!cfg.exception_handler_func_hints.empty()) {
    std::sort(cfg.exception_handler_func_hints.begin(), cfg.exception_handler_func_hints.end());
    cfg.exception_handler_func_hints.erase(std::unique(cfg.exception_handler_func_hints.begin(),
                                                       cfg.exception_handler_func_hints.end()),
                                           cfg.exception_handler_func_hints.end());
  }

  bool ok = true;
  if (cfg.file_path.empty()) {
    REXLOG_ERROR(codegen, "Missing required field: file_path");
    ok = false;
  }

  auto result = cfg.Validate();
  for (const auto& warning : result.warnings) {
    REXLOG_WARN(codegen, "config {}", warning);
  }
  for (const auto& error : result.errors) {
    REXLOG_ERROR(codegen, "config {}", error);
  }
  if (!result.valid) {
    ok = false;
  }
  return ok;
}

}  // namespace

bool RecompilerConfig::Load(const std::string_view& config_file_path) {
  std::set<std::string> visited;
  std::filesystem::path path(config_file_path);
  if (!LoadRecursive(path, *this, visited, 0)) {
    return false;
  }
  return FinalizeConfig(*this);
}

bool RecompilerConfig::LoadFromTable(const toml::table& tbl,
                                     const std::filesystem::path& base_dir) {
  std::set<std::string> visited;
  if (!ApplyTableWithIncludes(tbl, base_dir, *this, visited, 0, "<inline>")) {
    return false;
  }
  return FinalizeConfig(*this);
}

bool RecompilerConfig::LoadShared(const toml::table& tbl, const std::filesystem::path& base_dir) {
  std::set<std::string> visited;
  return ApplyTableWithIncludes(tbl, base_dir, *this, visited, 0, "<shared>");
}

void RecompilerConfig::ClearAddressEntries() {
  functions.clear();
  switch_tables.clear();
  mid_asm_hooks.clear();
  longjmp_address = 0;
  setjmp_address = 0;
  shim_functions.clear();
  invalid_instruction_hints.clear();
  known_indirect_call_hints.clear();
  declared_mmio_accesses.clear();
  exception_handler_func_hints.clear();
}

namespace {

bool InImage(u32 address, u32 base, u32 size) {
  return address >= base && u64{address} < u64{base} + size;
}

template <typename Map>
void MergeKeyedIn(Map& into, const Map& from, u32 base, u32 size) {
  for (const auto& [address, value] : from) {
    if (InImage(address, base, size)) {
      into.try_emplace(address, value);
    }
  }
}

}  // namespace

void RecompilerConfig::MergeSharedEntriesIn(const RecompilerConfig& shared, u32 base, u32 size) {
  MergeKeyedIn(functions, shared.functions, base, size);
  MergeKeyedIn(switch_tables, shared.switch_tables, base, size);
  MergeKeyedIn(mid_asm_hooks, shared.mid_asm_hooks, base, size);
  MergeKeyedIn(invalid_instruction_hints, shared.invalid_instruction_hints, base, size);
  MergeKeyedIn(declared_mmio_accesses, shared.declared_mmio_accesses, base, size);
  if (longjmp_address == 0 && InImage(shared.longjmp_address, base, size) &&
      shared.longjmp_address != 0) {
    longjmp_address = shared.longjmp_address;
  }
  if (setjmp_address == 0 && InImage(shared.setjmp_address, base, size) &&
      shared.setjmp_address != 0) {
    setjmp_address = shared.setjmp_address;
  }
  for (const auto& [name, address] : shared.shim_functions) {
    if (InImage(address, base, size)) {
      shim_functions.try_emplace(name, address);
    }
  }
  for (u32 address : shared.known_indirect_call_hints) {
    if (InImage(address, base, size)) {
      known_indirect_call_hints.insert(address);
    }
  }
  for (u32 address : shared.exception_handler_func_hints) {
    if (InImage(address, base, size) &&
        std::find(exception_handler_func_hints.begin(), exception_handler_func_hints.end(),
                  address) == exception_handler_func_hints.end()) {
      exception_handler_func_hints.push_back(address);
    }
  }
  std::sort(exception_handler_func_hints.begin(), exception_handler_func_hints.end());
}

std::vector<std::string> RecompilerConfig::ListEntriesOutside(
    const std::vector<std::pair<u32, u32>>& ranges) const {
  std::vector<std::string> outside;
  auto check = [&](std::string_view key, u32 address) {
    for (const auto& [base, size] : ranges) {
      if (InImage(address, base, size)) {
        return;
      }
    }
    outside.push_back(std::format("{} 0x{:08X}", key, address));
  };
  for (const auto& [address, value] : functions) {
    check("[functions]", address);
  }
  for (const auto& [address, value] : switch_tables) {
    check("[[switch_tables]]", address);
  }
  for (const auto& [address, value] : mid_asm_hooks) {
    check("[[midasm_hook]]", address);
  }
  for (const auto& [address, value] : invalid_instruction_hints) {
    check("[[invalid_instructions]]", address);
  }
  for (const auto& [address, value] : declared_mmio_accesses) {
    check("[mmio]", address);
  }
  for (const auto& [name, address] : shim_functions) {
    check("[shim]", address);
  }
  for (u32 address : known_indirect_call_hints) {
    check("indirect_calls", address);
  }
  for (u32 address : exception_handler_func_hints) {
    check("analysis.exception_handler_funcs", address);
  }
  if (longjmp_address != 0) {
    check("longjmp_address", longjmp_address);
  }
  if (setjmp_address != 0) {
    check("setjmp_address", setjmp_address);
  }
  std::sort(outside.begin(), outside.end());
  return outside;
}

RecompilerConfig::ValidationResult RecompilerConfig::Validate() const {
  ValidationResult result;

  auto check_alignment = [&](u32 addr, const char* name) {
    if (addr != 0 && (addr & 0x3) != 0) {
      result.errors.push_back(std::format("{} address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
  };

  check_alignment(longjmp_address, "longjmp");
  check_alignment(setjmp_address, "setjmp");
  for (const auto& entry : declared_mmio_accesses) {
    check_alignment(entry.first, "mmio");
  }

  for (const auto& [name, addr] : shim_functions) {
    if (addr & 0x3) {
      result.errors.push_back(
          std::format("shim function '{}' address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
    if (!rex::kernel::shim::IsRegistered(name)) {
      const auto nearest = NearestShimName(name);
      result.errors.push_back(
          nearest.empty()
              ? std::format("shim function '{}' is not in the shim registry", name)
              : std::format("shim function '{}' is not in the shim registry, did you mean '{}'?",
                            name, nearest));
      result.valid = false;
    }
  }

  for (const auto& [addr, hook] : mid_asm_hooks) {
    if (std::ranges::contains(hook.registers, "r1")) {
      result.errors.push_back(std::format(
          "midasm hook '{}' at 0x{:08X} takes r1, which the hook call moves under the red zone",
          hook.name, addr));
      result.valid = false;
    }
  }

  for (const auto& [addr, size] : functions) {
    if (addr & 0x3) {
      result.errors.push_back(std::format("Function address 0x{:08X} is not 4-byte aligned", addr));
      result.valid = false;
    }
  }

  {
    std::map<u32, u32> seen;
    for (const auto& [addr, cfg] : functions) {
      u32 func_size = cfg.GetSize(addr);
      auto it = seen.find(addr);
      if (it != seen.end()) {
        if (it->second == func_size) {
          result.errors.push_back(
              std::format("Duplicate function boundary: 0x{:08X} size 0x{:X}", addr, func_size));
        } else {
          result.errors.push_back(std::format("Conflicting sizes at 0x{:08X}: 0x{:X} vs 0x{:X}",
                                              addr, it->second, func_size));
        }
        result.valid = false;
      }
      seen[addr] = func_size;
    }
  }

  {
    std::vector<std::pair<u32, u32>> sorted;
    for (const auto& [addr, cfg] : functions) {
      if (!cfg.IsChunk()) {
        sorted.emplace_back(addr, cfg.GetSize(addr));
      }
    }
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
      u32 prev_end = sorted[i - 1].first + sorted[i - 1].second;
      if (sorted[i].first < prev_end) {
        result.errors.push_back(std::format(
            "Overlapping boundaries: 0x{:08X}+0x{:X} overlaps 0x{:08X}+0x{:X}", sorted[i - 1].first,
            sorted[i - 1].second, sorted[i].first, sorted[i].second));
        result.valid = false;
      }
    }
  }

  for (const auto& traits : rex::kernel::shim::kGroups) {
    if (!traits.atomic) {
      continue;
    }
    size_t found = 0;
    size_t total = 0;
    std::string missing;
    for (const auto& entry : rex::kernel::shim::kRegistry) {
      if (entry.group != traits.id) {
        continue;
      }
      ++total;
      if (shim_functions.contains(std::string(entry.name))) {
        ++found;
      } else {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += entry.name;
      }
    }
    if (found > 0 && found < total) {
      result.errors.push_back(
          std::format("[shim] {} group is incomplete ({}/{} specified), missing: {}", traits.name,
                      found, total, missing));
      result.valid = false;
    }
  }

  if (file_path.empty()) {
    result.warnings.push_back("file_path is empty");
  }
  if (out_directory_path.empty()) {
    result.warnings.push_back("out_directory_path is empty");
  }

  return result;
}

bool RecompilerConfig::UsesShimHeap() const {
  return std::ranges::any_of(rex::kernel::shim::kRegistry, [this](const auto& entry) {
    return rex::HasFlags(entry.flags, rex::kernel::shim::Flag::kRequiresHeap) &&
           shim_functions.contains(std::string(entry.name));
  });
}

}  // namespace rex::codegen
