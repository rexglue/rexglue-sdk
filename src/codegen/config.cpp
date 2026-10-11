#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <vector>

#include <format>

#include <rex/codegen/config.h>
#include <rex/kernel/shim/registry.h>
#include <rex/logging.h>

#include <toml++/toml.hpp>

namespace rex::codegen {

namespace {

/** Maximum nesting depth for include chains. */
constexpr u32 kMaxIncludeDepth = 32;

/**
 * Parses a hex address string with or without a "0x" or "0X" prefix.
 *
 * @param keyStr Address text.
 * @return The address, or nullopt when the text is not hex.
 */
std::optional<u32> ParseHexAddress(const std::string& keyStr) {
  try {
    if (keyStr.starts_with("0x") || keyStr.starts_with("0X")) {
      return static_cast<u32>(std::stoul(keyStr.substr(2), nullptr, 16));
    } else {
      return static_cast<u32>(std::stoul(keyStr, nullptr, 16));
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
  size_t bestDistance = kMaxDistance + 1;
  for (const auto& entry : rex::kernel::shim::kRegistry) {
    const size_t distance = EditDistance(name, entry.name);
    if (distance < bestDistance) {
      best = entry.name;
      bestDistance = distance;
    }
  }
  return best;
}

bool ApplyTOML(const toml::table& toml, RecompilerConfig& cfg, const std::string& filePath) {
  if (auto* retired = toml["rexcrt"].as_table()) {
    std::string converted = "[shim]";
    for (const auto& [name, val] : *retired) {
      if (auto addr = val.value<i64>()) {
        converted +=
            std::format("\n{} = 0x{:08X}", std::string(name.str()), static_cast<u32>(*addr));
      }
    }
    REXLOG_ERROR(codegen, "config {}: the [rexcrt] table was renamed, replace it with\n{}",
                 filePath, converted);
    return false;
  }

  if (auto v = toml["project_name"].value<std::string>()) {
    MergeScalar(cfg.projectName, *v, "project_name");
  }
  if (auto v = toml["file_path"].value<std::string>()) {
    MergeScalar(cfg.filePath, *v, "file_path");
  }
  if (auto v = toml["out_directory_path"].value<std::string>()) {
    MergeScalar(cfg.outDirectoryPath, *v, "out_directory_path");
  }
  if (auto v = toml["template_dir"].value<std::string>()) {
    MergeScalar(cfg.templateDir, *v, "template_dir");
  }

  auto hasBool = [&](const char* key) -> bool { return toml[key].is_boolean(); };
  MergeBool(cfg.skipLr, toml["skip_lr"].value_or(false), hasBool("skip_lr"), "skip_lr");
  MergeBool(cfg.skipMsr, toml["skip_msr"].value_or(false), hasBool("skip_msr"), "skip_msr");
  MergeBool(cfg.ctrAsLocalVariable, toml["ctr_as_local"].value_or(false), hasBool("ctr_as_local"),
            "ctr_as_local");
  MergeBool(cfg.xerAsLocalVariable, toml["xer_as_local"].value_or(false), hasBool("xer_as_local"),
            "xer_as_local");
  MergeBool(cfg.reservedRegisterAsLocalVariable, toml["reserved_as_local"].value_or(false),
            hasBool("reserved_as_local"), "reserved_as_local");
  MergeBool(cfg.crRegistersAsLocalVariables, toml["cr_as_local"].value_or(false),
            hasBool("cr_as_local"), "cr_as_local");
  MergeBool(cfg.nonArgumentRegistersAsLocalVariables, toml["non_argument_as_local"].value_or(false),
            hasBool("non_argument_as_local"), "non_argument_as_local");
  MergeBool(cfg.nonVolatileRegistersAsLocalVariables, toml["non_volatile_as_local"].value_or(false),
            hasBool("non_volatile_as_local"), "non_volatile_as_local");
  MergeBool(cfg.generateExceptionHandlers, toml["generate_exception_handlers"].value_or(false),
            hasBool("generate_exception_handlers"), "generate_exception_handlers");
  if (hasBool("is_dll")) {
    cfg.isDll = toml["is_dll"].value_or(false);
  }

  if (auto v = toml["longjmp_address"].value<i64>()) {
    u32 addr = static_cast<u32>(*v);
    MergeScalar(cfg.longJmpAddress, addr, "longjmp_address");
  }
  if (auto v = toml["setjmp_address"].value<i64>()) {
    u32 addr = static_cast<u32>(*v);
    MergeScalar(cfg.setJmpAddress, addr, "setjmp_address");
  }

  if (auto* analysisTable = toml["analysis"].as_table()) {
    if (auto v = (*analysisTable)["max_jump_extension"].value<u32>()) {
      MergeScalar(cfg.maxJumpExtension, *v, "analysis.max_jump_extension");
    }
    if (auto v = (*analysisTable)["data_region_threshold"].value<u32>()) {
      MergeScalar(cfg.dataRegionThreshold, *v, "analysis.data_region_threshold");
    }
    if (auto v = (*analysisTable)["large_function_threshold"].value<u32>()) {
      MergeScalar(cfg.largeFunctionThreshold, *v, "analysis.large_function_threshold");
    }

    if (auto handlers = (*analysisTable)["exception_handler_funcs"].as_array()) {
      for (const auto& elem : *handlers) {
        if (auto val = elem.value<i64>()) {
          cfg.exceptionHandlerFuncHints.push_back(static_cast<u32>(*val));
        }
      }
    }
  }

  if (auto* shimTable = toml["shim"].as_table()) {
    size_t added = 0;
    for (const auto& [name, val] : *shimTable) {
      if (auto addr = val.value<i64>()) {
        auto key = std::string(name);
        auto it = cfg.shimFunctions.find(key);
        if (it != cfg.shimFunctions.end()) {
          REXLOG_DEBUG(codegen, "config   [shim] {} overridden: 0x{:08X} -> 0x{:08X}", key,
                       it->second, static_cast<u32>(*addr));
        }
        cfg.shimFunctions[key] = static_cast<u32>(*addr);
        ++added;
      }
    }
    if (added > 0) {
      REXLOG_DEBUG(codegen, "config   [shim] added/updated {} entries from {}", added, filePath);
    }
  }

  if (auto functionsTable = toml["functions"].as_table()) {
    size_t added = 0;
    for (auto& [key, value] : *functionsTable) {
      std::string keyStr(key.str());
      auto addrOpt = ParseHexAddress(keyStr);
      if (!addrOpt) {
        REXLOG_ERROR(codegen, "Invalid function address key: {}", keyStr);
        continue;
      }
      u32 address = *addrOpt;

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
      fcfg.shareRegisters = (*table)["share_registers"].value_or(false);

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
                     filePath);
      }
      cfg.functions.insert_or_assign(address, std::move(fcfg));
      ++added;
    }
    if (added > 0) {
      REXLOG_DEBUG(codegen, "config   [functions] added/updated {} entries from {}", added,
                   filePath);
    }
  }

  if (auto invalidArray = toml["invalid_instructions"].as_array()) {
    for (auto& entry : *invalidArray) {
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

      auto it = cfg.invalidInstructionHints.find(*data_opt);
      if (it != cfg.invalidInstructionHints.end()) {
        REXLOG_DEBUG(codegen, "config   [[invalid_instructions]] 0x{:08X} overridden from {}",
                     *data_opt, filePath);
      }
      cfg.invalidInstructionHints.insert_or_assign(*data_opt, *size_opt);
    }
  }

  if (auto switchTableArray = toml["switch_tables"].as_array()) {
    for (auto& entry : *switchTableArray) {
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
      jt.bctrAddress = *address_opt;
      jt.tableAddress = 0;
      jt.indexRegister = static_cast<u8>(*register_opt);

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

      auto it = cfg.switchTables.find(*address_opt);
      if (it != cfg.switchTables.end()) {
        REXLOG_DEBUG(codegen, "config   [[switch_tables]] 0x{:08X} overridden from {}",
                     *address_opt, filePath);
      }
      size_t label_count = jt.targets.size();
      cfg.switchTables.insert_or_assign(*address_opt, std::move(jt));
      REXLOG_DEBUG(codegen, "Loaded manual switch table at 0x{:08X} with {} labels", *address_opt,
                   label_count);
    }
  }

  if (auto midAsmHookArray = toml["midasm_hook"].as_array()) {
    for (auto& entry : *midAsmHookArray) {
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

      MidAsmHook midAsmHook;
      midAsmHook.name = *name_opt;

      if (auto registerArray = (*table)["registers"].as_array()) {
        for (auto& reg : *registerArray) {
          if (auto reg_str = reg.value<std::string>()) {
            midAsmHook.registers.push_back(*reg_str);
          }
        }
      }

      midAsmHook.ret = (*table)["return"].value_or(false);
      midAsmHook.returnOnTrue = (*table)["return_on_true"].value_or(false);
      midAsmHook.returnOnFalse = (*table)["return_on_false"].value_or(false);

      midAsmHook.jumpAddress = (*table)["jump_address"].value_or(0u);
      midAsmHook.jumpAddressOnTrue = (*table)["jump_address_on_true"].value_or(0u);
      midAsmHook.jumpAddressOnFalse = (*table)["jump_address_on_false"].value_or(0u);

      if ((midAsmHook.ret && midAsmHook.jumpAddress != 0) ||
          (midAsmHook.returnOnTrue && midAsmHook.jumpAddressOnTrue != 0) ||
          (midAsmHook.returnOnFalse && midAsmHook.jumpAddressOnFalse != 0)) {
        REXLOG_ERROR(codegen, "{}: can't return and jump at the same time", midAsmHook.name);
      }

      if ((midAsmHook.ret || midAsmHook.jumpAddress != 0) &&
          (midAsmHook.returnOnFalse || midAsmHook.returnOnTrue ||
           midAsmHook.jumpAddressOnFalse != 0 || midAsmHook.jumpAddressOnTrue != 0)) {
        REXLOG_ERROR(codegen, "{}: can't mix direct and conditional return/jump", midAsmHook.name);
      }

      midAsmHook.afterInstruction = (*table)["after_instruction"].value_or(false);

      auto it = cfg.midAsmHooks.find(*address_opt);
      if (it != cfg.midAsmHooks.end()) {
        REXLOG_DEBUG(codegen, "config   [[midasm_hook]] 0x{:08X} overridden from {}", *address_opt,
                     filePath);
      }
      cfg.midAsmHooks.insert_or_assign(*address_opt, std::move(midAsmHook));
    }
  }

  if (auto indirectCallArray = toml["indirect_calls"].as_array()) {
    for (auto& entry : *indirectCallArray) {
      if (auto addr = entry.value<i64>()) {
        cfg.knownIndirectCallHints.insert(static_cast<u32>(*addr));
        REXLOG_DEBUG(codegen, "Loaded known indirect call hint at 0x{:08X}", *addr);
      }
    }
  }
  for (const MMIOBlock block : {MMIOBlock::GPU, MMIOBlock::APU}) {
    const std::string_view block_name = block == MMIOBlock::GPU ? "gpu" : "apu";
    for (const bool write : {false, true}) {
      const std::string_view list_name = write ? "write" : "read";
      const auto* list = toml["mmio"][block_name][list_name].as_array();
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
        if (!cfg.declaredMMIOAccesses.try_emplace(static_cast<u32>(*addr), block, write).second) {
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

bool LoadRecursive(const std::filesystem::path& filePath, RecompilerConfig& cfg,
                   std::set<std::string>& visited, u32 depth) {
  if (depth > kMaxIncludeDepth) {
    REX_FAIL(codegen, false, "config include depth exceeds maximum ({}) at: {}", kMaxIncludeDepth,
             filePath.string());
  }

  std::error_code ec;
  auto canonical = std::filesystem::canonical(filePath, ec);
  if (ec) {
    REX_FAIL(codegen, false, "config cannot resolve path '{}': {}", filePath.string(),
             ec.message());
  }
  std::string canonicalStr = canonical.string();
  if (visited.contains(canonicalStr)) {
    REX_FAIL(codegen, false, "config circular include detected: {}", canonicalStr);
  }
  visited.insert(canonicalStr);
  cfg.loadedFiles.push_back(canonicalStr);

  toml::table toml;
  try {
    toml = toml::parse_file(canonicalStr);
  } catch (const toml::parse_error& e) {
    REX_FAIL(codegen, false, "Failed to parse config file '{}': {}", canonicalStr, e.what());
  }
  REXLOG_DEBUG(codegen, "config loaded: {}", filePath.filename().string());
  return ApplyTableWithIncludes(toml, canonical.parent_path(), cfg, visited, depth,
                                filePath.filename().string());
}

bool ApplyTableWithIncludes(const toml::table& tbl, const std::filesystem::path& base_dir,
                            RecompilerConfig& cfg, std::set<std::string>& visited, u32 depth,
                            const std::string& description) {
  // Process includes first (depth-first), so this table's own values win.
  if (auto* includesArray = tbl["includes"].as_array()) {
    for (const auto& elem : *includesArray) {
      if (auto includePath = elem.value<std::string>()) {
        auto resolved = base_dir / *includePath;
        if (!std::filesystem::exists(resolved)) {
          REX_FAIL(codegen, false, "config included file not found: {} (resolved from {})",
                   *includePath, description);
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
      if (fc.isChunk())
        chunks_count++;
    }
    REXLOG_TRACE(codegen, "Loaded {} function configs ({} standalone, {} chunks)",
                 cfg.functions.size(), cfg.functions.size() - chunks_count, chunks_count);
  }
  if (!cfg.exceptionHandlerFuncHints.empty()) {
    std::sort(cfg.exceptionHandlerFuncHints.begin(), cfg.exceptionHandlerFuncHints.end());
    cfg.exceptionHandlerFuncHints.erase(
        std::unique(cfg.exceptionHandlerFuncHints.begin(), cfg.exceptionHandlerFuncHints.end()),
        cfg.exceptionHandlerFuncHints.end());
  }

  bool ok = true;
  if (cfg.filePath.empty()) {
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

bool RecompilerConfig::Load(const std::string_view& configFilePath) {
  std::set<std::string> visited;
  std::filesystem::path path(configFilePath);
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

RecompilerConfig::ValidationResult RecompilerConfig::Validate() const {
  ValidationResult result;

  auto checkAlignment = [&](u32 addr, const char* name) {
    if (addr != 0 && (addr & 0x3) != 0) {
      result.errors.push_back(std::format("{} address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
  };

  checkAlignment(longJmpAddress, "longjmp");
  checkAlignment(setJmpAddress, "setjmp");
  for (const auto& entry : declaredMMIOAccesses) {
    checkAlignment(entry.first, "mmio");
  }

  for (const auto& [name, addr] : shimFunctions) {
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

  for (const auto& [addr, hook] : midAsmHooks) {
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
      u32 funcSize = cfg.getSize(addr);
      auto it = seen.find(addr);
      if (it != seen.end()) {
        if (it->second == funcSize) {
          result.errors.push_back(
              std::format("Duplicate function boundary: 0x{:08X} size 0x{:X}", addr, funcSize));
        } else {
          result.errors.push_back(std::format("Conflicting sizes at 0x{:08X}: 0x{:X} vs 0x{:X}",
                                              addr, it->second, funcSize));
        }
        result.valid = false;
      }
      seen[addr] = funcSize;
    }
  }

  {
    std::vector<std::pair<u32, u32>> sorted;
    for (const auto& [addr, cfg] : functions) {
      if (!cfg.isChunk()) {
        sorted.emplace_back(addr, cfg.getSize(addr));
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
      if (shimFunctions.contains(std::string(entry.name))) {
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

  if (filePath.empty()) {
    result.warnings.push_back("file_path is empty");
  }
  if (outDirectoryPath.empty()) {
    result.warnings.push_back("out_directory_path is empty");
  }

  return result;
}

bool RecompilerConfig::UsesShimHeap() const {
  return std::ranges::any_of(rex::kernel::shim::kRegistry, [this](const auto& entry) {
    return rex::HasFlags(entry.flags, rex::kernel::shim::Flag::RequiresHeap) &&
           shimFunctions.contains(std::string(entry.name));
  });
}

}  // namespace rex::codegen
