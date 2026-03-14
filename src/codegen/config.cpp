/**
 * @file        rexcodegen/config.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp toml config
 */

// TOML config file loading

#include <algorithm>
#include <map>
#include <set>

#include <fmt/format.h>

#include <rex/codegen/config.h>
#include <rex/logging.h>

#include "codegen_logging.h"

#include <toml++/toml.hpp>

namespace rex::codegen {

namespace {

/// Maximum nesting depth for include chains.
constexpr uint32_t kMaxIncludeDepth = 32;

/// Parse a hex address string (with or without "0x"/"0X" prefix).
std::optional<uint32_t> ParseHexAddress(const std::string& keyStr) {
  try {
    if (keyStr.starts_with("0x") || keyStr.starts_with("0X")) {
      return static_cast<uint32_t>(std::stoul(keyStr.substr(2), nullptr, 16));
    } else {
      return static_cast<uint32_t>(std::stoul(keyStr, nullptr, 16));
    }
  } catch (...) {
    return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Scalar merge helpers -- log overrides at debug level
// ---------------------------------------------------------------------------

template <typename T>
void MergeScalar(T& dst, const T& src, const char* name) {
  if (src != T{} && src != dst) {
    if (dst != T{}) {
      if constexpr (std::is_same_v<T, bool>) {
        REXCODEGEN_DEBUG("[config]   {}: {} -> {} (overridden)", name, dst, src);
      } else if constexpr (std::is_same_v<T, std::string>) {
        REXCODEGEN_DEBUG("[config]   {}: \"{}\" -> \"{}\" (overridden)", name, dst, src);
      } else {
        REXCODEGEN_DEBUG("[config]   {}: {} -> {} (overridden)", name, dst, src);
      }
    }
    dst = src;
  }
}

/// Overload for booleans where the "zero" state (false) is meaningful.
/// Only skip the merge when the overlay explicitly did not set the key.
void MergeBool(bool& dst, bool src, bool present, const char* name) {
  if (!present)
    return;
  if (src != dst) {
    REXCODEGEN_DEBUG("[config]   {}: {} -> {} (overridden)", name, dst ? "true" : "false",
                     src ? "true" : "false");
  }
  dst = src;
}

// ---------------------------------------------------------------------------
// Apply a single parsed TOML table onto the config (merge semantics)
// ---------------------------------------------------------------------------

void ApplyToml(const toml::table& toml, RecompilerConfig& cfg, const std::string& filePath) {
  // --- Scalars: last wins ---

  // String scalars (only override if present in this file)
  if (auto v = toml["project_name"].value<std::string>()) {
    MergeScalar(cfg.projectName, *v, "project_name");
  }
  if (auto v = toml["file_path"].value<std::string>()) {
    MergeScalar(cfg.filePath, *v, "file_path");
  }
  if (auto v = toml["out_directory_path"].value<std::string>()) {
    MergeScalar(cfg.outDirectoryPath, *v, "out_directory_path");
  }
  if (auto v = toml["patch_file_path"].value<std::string>()) {
    MergeScalar(cfg.patchFilePath, *v, "patch_file_path");
  }
  if (auto v = toml["patched_file_path"].value<std::string>()) {
    MergeScalar(cfg.patchedFilePath, *v, "patched_file_path");
  }

  // Bool scalars
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

  // Integer scalars (only override if present)
  if (auto v = toml["longjmp_address"].value<int64_t>()) {
    uint32_t addr = static_cast<uint32_t>(*v);
    MergeScalar(cfg.longJmpAddress, addr, "longjmp_address");
  }
  if (auto v = toml["setjmp_address"].value<int64_t>()) {
    uint32_t addr = static_cast<uint32_t>(*v);
    MergeScalar(cfg.setJmpAddress, addr, "setjmp_address");
  }

  // --- [analysis] section scalars ---
  if (auto* analysisTable = toml["analysis"].as_table()) {
    if (auto v = (*analysisTable)["max_jump_extension"].value<uint32_t>()) {
      MergeScalar(cfg.maxJumpExtension, *v, "analysis.max_jump_extension");
    }
    if (auto v = (*analysisTable)["data_region_threshold"].value<uint32_t>()) {
      MergeScalar(cfg.dataRegionThreshold, *v, "analysis.data_region_threshold");
    }
    if (auto v = (*analysisTable)["large_function_threshold"].value<uint32_t>()) {
      MergeScalar(cfg.largeFunctionThreshold, *v, "analysis.large_function_threshold");
    }

    // exceptionHandlerFuncHints -- push_back then deduplicate at end
    if (auto handlers = (*analysisTable)["exception_handler_funcs"].as_array()) {
      for (const auto& elem : *handlers) {
        if (auto val = elem.value<int64_t>()) {
          cfg.exceptionHandlerFuncHints.push_back(static_cast<uint32_t>(*val));
        }
      }
    }
  }

  // --- Keyed tables: additive, same key = last wins ---

  // [rexcrt]
  if (auto* rexcrt = toml["rexcrt"].as_table()) {
    size_t added = 0;
    for (const auto& [name, val] : *rexcrt) {
      if (auto addr = val.value<int64_t>()) {
        auto key = std::string(name);
        auto it = cfg.rexcrtFunctions.find(key);
        if (it != cfg.rexcrtFunctions.end()) {
          REXCODEGEN_DEBUG("[config]   [rexcrt] {} overridden: 0x{:08X} -> 0x{:08X}", key,
                           it->second, static_cast<uint32_t>(*addr));
        }
        cfg.rexcrtFunctions[key] = static_cast<uint32_t>(*addr);
        ++added;
      }
    }
    if (added > 0) {
      REXCODEGEN_DEBUG("[config]   [rexcrt] added/updated {} entries from {}", added, filePath);
    }
  }

  // [functions]
  if (auto functionsTable = toml["functions"].as_table()) {
    size_t added = 0;
    for (auto& [key, value] : *functionsTable) {
      std::string keyStr(key.str());
      auto addrOpt = ParseHexAddress(keyStr);
      if (!addrOpt) {
        REXCODEGEN_ERROR("Invalid function address key: {}", keyStr);
        continue;
      }
      uint32_t address = *addrOpt;

      auto* table = value.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [functions] entry at 0x{:08X}: expected table", address);
        continue;
      }

      FunctionConfig fcfg;
      fcfg.size = (*table)["size"].value_or(0u);
      fcfg.end = (*table)["end"].value_or(0u);
      fcfg.name = (*table)["name"].value_or(std::string{});
      fcfg.parent = (*table)["parent"].value_or(0u);

      if (fcfg.size && fcfg.end) {
        REXCODEGEN_ERROR("Function 0x{:08X}: cannot specify both 'size' and 'end'", address);
        continue;
      }
      if (fcfg.end && fcfg.end <= address) {
        REXCODEGEN_ERROR("Function 0x{:08X}: 'end' (0x{:08X}) must be greater than address",
                         address, fcfg.end);
        continue;
      }

      auto it = cfg.functions.find(address);
      if (it != cfg.functions.end()) {
        REXCODEGEN_DEBUG("[config]   [functions] 0x{:08X} overridden from {}", address, filePath);
      }
      cfg.functions.insert_or_assign(address, std::move(fcfg));
      ++added;
    }
    if (added > 0) {
      REXCODEGEN_DEBUG("[config]   [functions] added/updated {} entries from {}", added, filePath);
    }
  }

  // --- Arrays of tables: deduplicated by primary key (address), last wins ---

  // [[invalid_instructions]] -- keyed by "data" address
  if (auto invalidArray = toml["invalid_instructions"].as_array()) {
    for (auto& entry : *invalidArray) {
      auto* table = entry.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [[invalid_instructions]] entry: expected table");
        continue;
      }

      auto data_opt = (*table)["data"].value<uint32_t>();
      auto size_opt = (*table)["size"].value<uint32_t>();

      if (!data_opt) {
        REXCODEGEN_ERROR("Missing 'data' in [[invalid_instructions]] entry");
        continue;
      }
      if (!size_opt) {
        REXCODEGEN_ERROR("Missing 'size' in [[invalid_instructions]] entry");
        continue;
      }

      auto it = cfg.invalidInstructionHints.find(*data_opt);
      if (it != cfg.invalidInstructionHints.end()) {
        REXCODEGEN_DEBUG("[config]   [[invalid_instructions]] 0x{:08X} overridden from {}",
                         *data_opt, filePath);
      }
      cfg.invalidInstructionHints.insert_or_assign(*data_opt, *size_opt);
    }
  }

  // [[switch_tables]] -- keyed by "address"
  if (auto switchTableArray = toml["switch_tables"].as_array()) {
    for (auto& entry : *switchTableArray) {
      auto* table = entry.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [[switch_tables]] entry: expected table");
        continue;
      }

      auto address_opt = (*table)["address"].value<uint32_t>();
      auto register_opt = (*table)["register"].value<uint32_t>();
      auto labels_array = (*table)["labels"].as_array();

      if (!address_opt) {
        REXCODEGEN_ERROR("Missing 'address' in [[switch_tables]] entry");
        continue;
      }
      if (!register_opt) {
        REXCODEGEN_ERROR("Missing 'register' in [[switch_tables]] entry");
        continue;
      }
      if (!labels_array) {
        REXCODEGEN_ERROR("Missing 'labels' in [[switch_tables]] entry");
        continue;
      }

      JumpTable jt;
      jt.bctrAddress = *address_opt;
      jt.tableAddress = 0;
      jt.indexRegister = static_cast<uint8_t>(*register_opt);

      for (auto& label : *labels_array) {
        if (auto label_val = label.value<int64_t>()) {
          jt.targets.push_back(static_cast<uint32_t>(*label_val));
        }
      }

      if (jt.targets.empty()) {
        REXCODEGEN_ERROR("Empty 'labels' array in [[switch_tables]] at 0x{:08X}", *address_opt);
        continue;
      }

      auto it = cfg.switchTables.find(*address_opt);
      if (it != cfg.switchTables.end()) {
        REXCODEGEN_DEBUG("[config]   [[switch_tables]] 0x{:08X} overridden from {}", *address_opt,
                         filePath);
      }
      size_t label_count = jt.targets.size();
      cfg.switchTables.insert_or_assign(*address_opt, std::move(jt));
      REXCODEGEN_DEBUG("Loaded manual switch table at 0x{:08X} with {} labels", *address_opt,
                       label_count);
    }
  }

  // [[midasm_hook]] -- keyed by "address"
  if (auto midAsmHookArray = toml["midasm_hook"].as_array()) {
    for (auto& entry : *midAsmHookArray) {
      auto* table = entry.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [[midasm_hook]] entry: expected table");
        continue;
      }

      auto address_opt = (*table)["address"].value<uint32_t>();
      auto name_opt = (*table)["name"].value<std::string>();

      if (!address_opt) {
        REXCODEGEN_ERROR("Missing 'address' in [[midasm_hook]] entry");
        continue;
      }
      if (!name_opt) {
        REXCODEGEN_ERROR("Missing 'name' in [[midasm_hook]] entry");
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
        REXCODEGEN_ERROR("{}: can't return and jump at the same time", midAsmHook.name);
      }

      if ((midAsmHook.ret || midAsmHook.jumpAddress != 0) &&
          (midAsmHook.returnOnFalse || midAsmHook.returnOnTrue ||
           midAsmHook.jumpAddressOnFalse != 0 || midAsmHook.jumpAddressOnTrue != 0)) {
        REXCODEGEN_ERROR("{}: can't mix direct and conditional return/jump", midAsmHook.name);
      }

      midAsmHook.afterInstruction = (*table)["after_instruction"].value_or(false);

      auto it = cfg.midAsmHooks.find(*address_opt);
      if (it != cfg.midAsmHooks.end()) {
        REXCODEGEN_DEBUG("[config]   [[midasm_hook]] 0x{:08X} overridden from {}", *address_opt,
                         filePath);
      }
      cfg.midAsmHooks.insert_or_assign(*address_opt, std::move(midAsmHook));
    }
  }

  // --- Sets: additive ---

  // indirect_calls -> knownIndirectCallHints (set)
  if (auto indirectCallArray = toml["indirect_calls"].as_array()) {
    for (auto& entry : *indirectCallArray) {
      if (auto addr = entry.value<int64_t>()) {
        cfg.knownIndirectCallHints.insert(static_cast<uint32_t>(*addr));
        REXCODEGEN_DEBUG("Loaded known indirect call hint at 0x{:08X}", *addr);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Recursive include loader
// ---------------------------------------------------------------------------

bool LoadRecursive(const std::filesystem::path& filePath, RecompilerConfig& cfg,
                   std::set<std::string>& visited, uint32_t depth) {
  // Depth check
  if (depth > kMaxIncludeDepth) {
    REXCODEGEN_ERROR("[config] include depth exceeds maximum ({}) at: {}", kMaxIncludeDepth,
                     filePath.string());
    return false;
  }

  // Canonical path for cycle detection
  std::error_code ec;
  auto canonical = std::filesystem::canonical(filePath, ec);
  if (ec) {
    REXCODEGEN_ERROR("[config] cannot resolve path '{}': {}", filePath.string(), ec.message());
    return false;
  }
  std::string canonicalStr = canonical.string();

  // Circular include detection
  if (visited.contains(canonicalStr)) {
    REXCODEGEN_ERROR("[config] circular include detected: {}", canonicalStr);
    return false;
  }
  visited.insert(canonicalStr);

  // Parse this file
  toml::table toml;
  try {
    toml = toml::parse_file(canonicalStr);
  } catch (const toml::parse_error& e) {
    REXCODEGEN_ERROR("Failed to parse config file '{}': {}", canonicalStr, e.what());
    return false;
  }

  REXCODEGEN_DEBUG("[config] loaded: {}", filePath.filename().string());

  // Process includes first (depth-first), before applying this file's values.
  // This means this file's values override anything set by included files.
  auto parentDir = canonical.parent_path();

  if (auto* includesArray = toml["includes"].as_array()) {
    for (const auto& elem : *includesArray) {
      if (auto includePath = elem.value<std::string>()) {
        auto resolved = parentDir / *includePath;
        if (!std::filesystem::exists(resolved)) {
          REXCODEGEN_ERROR("[config] included file not found: {} (resolved from {})", *includePath,
                           canonicalStr);
          return false;
        }
        if (!LoadRecursive(resolved, cfg, visited, depth + 1)) {
          return false;
        }
      }
    }
  }

  // Apply this file's config (after includes, so this file wins)
  ApplyToml(toml, cfg, filePath.filename().string());

  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool RecompilerConfig::Load(const std::string_view& configFilePath) {
  std::set<std::string> visited;
  std::filesystem::path path(configFilePath);

  if (!LoadRecursive(path, *this, visited, 0)) {
    return false;
  }

  // Post-load summary logging
  if (!functions.empty()) {
    size_t chunks_count = 0;
    for (const auto& [addr, cfg] : functions) {
      if (cfg.isChunk())
        chunks_count++;
    }
    REXCODEGEN_INFO("Loaded {} function configs ({} standalone, {} chunks)", functions.size(),
                    functions.size() - chunks_count, chunks_count);
  }

  // Deduplicate exceptionHandlerFuncHints (push_back from multiple files)
  if (!exceptionHandlerFuncHints.empty()) {
    std::sort(exceptionHandlerFuncHints.begin(), exceptionHandlerFuncHints.end());
    exceptionHandlerFuncHints.erase(
        std::unique(exceptionHandlerFuncHints.begin(), exceptionHandlerFuncHints.end()),
        exceptionHandlerFuncHints.end());
  }

  // Required field check
  if (filePath.empty()) {
    REXCODEGEN_ERROR("Missing required field: file_path");
  }

  // Validate assembled config
  auto result = Validate();
  for (const auto& warning : result.warnings) {
    REXCODEGEN_WARN("[config] {}", warning);
  }
  for (const auto& error : result.errors) {
    REXCODEGEN_ERROR("[config] {}", error);
  }

  return true;
}

RecompilerConfig::ValidationResult RecompilerConfig::Validate() const {
  ValidationResult result;

  // Helper to check 4-byte alignment (PPC instructions are 4-byte aligned)
  auto checkAlignment = [&](uint32_t addr, const char* name) {
    if (addr != 0 && (addr & 0x3) != 0) {
      result.errors.push_back(fmt::format("{} address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
  };

  // Check special address alignment
  checkAlignment(longJmpAddress, "longjmp");
  checkAlignment(setJmpAddress, "setjmp");

  for (const auto& [name, addr] : rexcrtFunctions) {
    if (addr & 0x3) {
      result.errors.push_back(
          fmt::format("rexcrt function '{}' address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
  }

  // Check function address alignment
  for (const auto& [addr, size] : functions) {
    if (addr & 0x3) {
      result.errors.push_back(fmt::format("Function address 0x{:08X} is not 4-byte aligned", addr));
      result.valid = false;
    }
  }

  // Check for duplicate function boundaries
  {
    std::map<uint32_t, uint32_t> seen;
    for (const auto& [addr, cfg] : functions) {
      uint32_t funcSize = cfg.getSize(addr);
      auto it = seen.find(addr);
      if (it != seen.end()) {
        if (it->second == funcSize) {
          result.errors.push_back(
              fmt::format("Duplicate function boundary: 0x{:08X} size 0x{:X}", addr, funcSize));
        } else {
          result.errors.push_back(fmt::format("Conflicting sizes at 0x{:08X}: 0x{:X} vs 0x{:X}",
                                              addr, it->second, funcSize));
        }
        result.valid = false;
      }
      seen[addr] = funcSize;
    }
  }

  // Check for overlapping function boundaries (standalone functions only)
  {
    std::vector<std::pair<uint32_t, uint32_t>> sorted;
    for (const auto& [addr, cfg] : functions) {
      if (!cfg.isChunk()) {
        sorted.emplace_back(addr, cfg.getSize(addr));
      }
    }
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
      uint32_t prev_end = sorted[i - 1].first + sorted[i - 1].second;
      if (sorted[i].first < prev_end) {
        result.errors.push_back(fmt::format(
            "Overlapping boundaries: 0x{:08X}+0x{:X} overlaps 0x{:08X}+0x{:X}", sorted[i - 1].first,
            sorted[i - 1].second, sorted[i].first, sorted[i].second));
        result.valid = false;
      }
    }
  }

  // Validate rexcrt all-or-nothing groups -- originals are stripped so partial
  // sets would leave the game with missing CRT functions at runtime.
  if (!rexcrtFunctions.empty()) {
    auto checkGroup = [&](const char* groupName, std::initializer_list<const char*> required) {
      size_t found = 0;
      for (const char* name : required) {
        if (rexcrtFunctions.contains(name))
          ++found;
      }
      if (found > 0 && found < required.size()) {
        std::string missing;
        for (const char* name : required) {
          if (!rexcrtFunctions.contains(name)) {
            if (!missing.empty())
              missing += ", ";
            missing += name;
          }
        }
        result.errors.push_back(
            fmt::format("[rexcrt] {} group is incomplete ({}/{} specified), missing: {}", groupName,
                        found, required.size(), missing));
        result.valid = false;
      }
    };

    checkGroup("heap", {"RtlAllocateHeap", "RtlFreeHeap", "RtlSizeHeap", "RtlReAllocateHeap"});
  }

  // Check required fields
  if (filePath.empty()) {
    result.warnings.push_back("file_path is empty");
  }
  if (outDirectoryPath.empty()) {
    result.warnings.push_back("out_directory_path is empty");
  }

  return result;
}

}  // namespace rex::codegen
